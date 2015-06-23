#include <algorithm>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "build-remote.hh"

#include "util.hh"
#include "misc.hh"
#include "serve-protocol.hh"
#include "worker-protocol.hh"

using namespace nix;


struct Child
{
    Pid pid;
    AutoCloseFD to, from;
};


static void openConnection(const string & sshName, const string & sshKey,
    int stderrFD, Child & child)
{
    Pipe to, from;
    to.create();
    from.create();

    child.pid = startProcess([&]() {

        if (dup2(to.readSide, STDIN_FILENO) == -1)
            throw SysError("cannot dup input pipe to stdin");

        if (dup2(from.writeSide, STDOUT_FILENO) == -1)
            throw SysError("cannot dup output pipe to stdout");

        if (dup2(stderrFD, STDERR_FILENO) == -1)
            throw SysError("cannot dup stderr");

        // FIXME: connection timeouts
        Strings argv(
            { "ssh", sshName, "-i", sshKey, "-x", "-a"
            , "-oBatchMode=yes", "-oConnectTimeout=60", "-oTCPKeepAlive=yes"
            , "--", "nix-store", "--serve", "--write" });

        execvp("ssh", (char * *) stringsToCharPtrs(argv).data()); // FIXME: remove cast

        throw SysError("cannot start ssh");
    });

    to.readSide.close();
    from.writeSide.close();

    child.to = to.writeSide.borrow();
    child.from = from.readSide.borrow();
}


static void copyClosureTo(std::shared_ptr<StoreAPI> store,
    FdSource & from, FdSink & to, const PathSet & paths,
    TokenServer & copyClosureTokenServer,
    bool useSubstitutes = false)
{
    PathSet closure;
    for (auto & path : paths)
        computeFSClosure(*store, path, closure);

    /* Send the "query valid paths" command with the "lock" option
       enabled. This prevents a race where the remote host
       garbage-collect paths that are already there. Optionally, ask
       the remote host to substitute missing paths. */
    writeInt(cmdQueryValidPaths, to);
    writeInt(1, to); // == lock paths
    writeInt(useSubstitutes, to);
    writeStrings(closure, to);
    to.flush();

    /* Get back the set of paths that are already valid on the remote
       host. */
    auto present = readStorePaths<PathSet>(from);

    if (present.size() == closure.size()) return;

    Paths sorted = topoSortPaths(*store, closure);

    Paths missing;
    for (auto i = sorted.rbegin(); i != sorted.rend(); ++i)
        if (present.find(*i) == present.end()) missing.push_back(*i);

    /* Ensure that only a limited number of threads can copy closures
       at the same time. However, proceed anyway after a timeout to
       prevent starvation by a handful of really huge closures. */
    time_t start = time(0);
    int timeout = 60 * (10 + rand() % 5);
    auto token(copyClosureTokenServer.get(timeout));
    time_t stop = time(0);

    if (token())
        printMsg(lvlDebug, format("got copy closure token after %1%s") % (stop - start));
    else
        printMsg(lvlDebug, format("did not get copy closure token after %1%s") % (stop - start));

    printMsg(lvlDebug, format("sending %1% missing paths") % missing.size());

    writeInt(cmdImportPaths, to);
    exportPaths(*store, missing, false, to);
    to.flush();

    if (readInt(from) != 1)
        throw Error("remote machine failed to import closure");
}


static void copyClosureFrom(std::shared_ptr<StoreAPI> store,
    FdSource & from, FdSink & to, const PathSet & paths)
{
    writeInt(cmdExportPaths, to);
    writeInt(0, to); // == don't sign
    writeStrings(paths, to);
    to.flush();
    store->importPaths(false, from);
}


void buildRemote(std::shared_ptr<StoreAPI> store,
    const string & sshName, const string & sshKey,
    const Path & drvPath, const Derivation & drv,
    const nix::Path & logDir, unsigned int maxSilentTime, unsigned int buildTimeout,
    TokenServer & copyClosureTokenServer,
    RemoteResult & result, counter & nrStepsBuilding)
{
    string base = baseNameOf(drvPath);
    result.logFile = logDir + "/" + string(base, 0, 2) + "/" + string(base, 2);
    AutoDelete autoDelete(result.logFile, false);

    createDirs(dirOf(result.logFile));

    AutoCloseFD logFD(open(result.logFile.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666));
    if (logFD == -1) throw SysError(format("creating log file ‘%1%’") % result.logFile);

    Child child;
    openConnection(sshName, sshKey, logFD, child);

    logFD.close();

    FdSource from(child.from);
    FdSink to(child.to);

    /* Handshake. */
    try {
        writeInt(SERVE_MAGIC_1, to);
        writeInt(SERVE_PROTOCOL_VERSION, to);
        to.flush();

        unsigned int magic = readInt(from);
        if (magic != SERVE_MAGIC_2)
            throw Error(format("protocol mismatch with ‘nix-store --serve’ on ‘%1%’") % sshName);
        unsigned int version = readInt(from);
        if (GET_PROTOCOL_MAJOR(version) != 0x200)
            throw Error(format("unsupported ‘nix-store --serve’ protocol version on ‘%1%’") % sshName);
    } catch (EndOfFile & e) {
        child.pid.wait(true);
        string s = chomp(readFile(result.logFile));
        throw Error(format("cannot connect to ‘%1%’: %2%") % sshName % s);
    }

    /* Gather the inputs. */
    PathSet inputs({drvPath});
    for (auto & input : drv.inputDrvs) {
        Derivation drv2 = readDerivation(input.first);
        for (auto & name : input.second) {
            auto i = drv2.outputs.find(name);
            if (i != drv2.outputs.end()) inputs.insert(i->second.path);
        }
    }

    /* Copy the input closure. */
    printMsg(lvlDebug, format("sending closure of ‘%1%’ to ‘%2%’") % drvPath % sshName);
    copyClosureTo(store, from, to, inputs, copyClosureTokenServer);

    autoDelete.cancel();

    /* Do the build. */
    printMsg(lvlDebug, format("building ‘%1%’ on ‘%2%’") % drvPath % sshName);
    writeInt(cmdBuildPaths, to);
    writeStrings(PathSet({drvPath}), to);
    writeInt(maxSilentTime, to);
    writeInt(buildTimeout, to);
    // FIXME: send maxLogSize.
    to.flush();
    result.startTime = time(0);
    int res;
    {
        MaintainCount mc(nrStepsBuilding);
        res = readInt(from);
    }
    result.stopTime = time(0);
    if (res) {
        result.errorMsg = (format("%1% on ‘%2%’") % readString(from) % sshName).str();
        if (res == 100) result.status = RemoteResult::rrPermanentFailure;
        else if (res == 101) result.status = RemoteResult::rrTimedOut;
        else result.status = RemoteResult::rrMiscFailure;
        return;
    }

    /* Copy the output paths. */
    printMsg(lvlDebug, format("copying outputs of ‘%1%’ from ‘%2%’") % drvPath % sshName);
    PathSet outputs;
    for (auto & output : drv.outputs)
        outputs.insert(output.second.path);
    copyClosureFrom(store, from, to, outputs);

    /* Shut down the connection. */
    child.to.close();
    child.pid.wait(true);

    result.status = RemoteResult::rrSuccess;
}
