/*
 * S2E Selective Symbolic Execution Framework
 *
 * Copyright (c) 2010, Dependable Systems Laboratory, EPFL
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Dependable Systems Laboratory, EPFL nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE DEPENDABLE SYSTEMS LABORATORY, EPFL BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Currently maintained by:
 *    Volodymyr Kuznetsov <vova.kuznetsov@epfl.ch>
 *    Vitaly Chipounov <vitaly.chipounov@epfl.ch>
 *
 * All contributors are listed in S2E-AUTHORS file.
 *
 */

// XXX: qemu stuff should be included before anything from KLEE or LLVM !
#include <tcg-llvm.h>

#include "S2E.h"

#include <s2e/Plugin.h>
#include <s2e/Plugins/CorePlugin.h>
#include <s2e/ConfigFile.h>
#include <s2e/Utils.h>
#include <s2e/S2EExecutor.h>
#include <s2e/S2EExecutionState.h>
#include <s2e/Database.h>

#include <s2e/s2e_qemu.h>

#include <llvm/System/Path.h>
#include <llvm/ModuleProvider.h>
#include <llvm/Module.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Bitcode/ReaderWriter.h>

#include <klee/Interpreter.h>
#include <klee/Common.h>

#include <iostream>
#include <sstream>
#include <deque>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>

#include <stdarg.h>
#include <stdio.h>

#include <sys/stat.h>

#ifndef _WIN32
#include <sys/types.h>
#include <unistd.h>
#endif

// stacktrace.h (c) 2008, Timo Bingmann from http://idlebox.net/
// published under the WTFPL v2.0
#if defined(CONFIG_WIN32)
void print_stacktrace(void)
{
    std::ostream &os = g_s2e->getDebugStream();
    os << "Stack trace printing unsupported on Windows" << std::endl;
}
#else
#include <stdio.h>
#include <stdlib.h>
#include <execinfo.h>
#include <cxxabi.h>

/** Print a demangled stack backtrace of the caller function to FILE* out. */
void print_stacktrace(void)
{
    unsigned int max_frames = 63;
    std::ostream &os = g_s2e->getDebugStream();
    os << "Stack trace" << std::endl;

    // storage array for stack trace address data
    void* addrlist[max_frames+1];

    // retrieve current stack addresses
    int addrlen = backtrace(addrlist, sizeof(addrlist) / sizeof(void*));

    if (addrlen == 0) {
        s2e_debug_print("  <empty, possibly corrupt>\n");
        return;
    }

    // resolve addresses into strings containing "filename(function+address)",
    // this array must be free()-ed
    char** symbollist = backtrace_symbols(addrlist, addrlen);

    // allocate string which will be filled with the demangled function name
    size_t funcnamesize = 256;
    char* funcname = (char*)malloc(funcnamesize);

    // iterate over the returned symbol lines. skip the first, it is the
    // address of this function.
    for (int i = 1; i < addrlen; i++)
    {
        char *begin_name = 0, *begin_offset = 0, *end_offset = 0;

        // find parentheses and +address offset surrounding the mangled name:
        // ./module(function+0x15c) [0x8048a6d]
        for (char *p = symbollist[i]; *p; ++p)
        {
            if (*p == '(')
                begin_name = p;
            else if (*p == '+')
                begin_offset = p;
            else if (*p == ')' && begin_offset) {
                end_offset = p;
                break;
            }
        }

        if (begin_name && begin_offset && end_offset
            && begin_name < begin_offset)
        {
            *begin_name++ = '\0';
            *begin_offset++ = '\0';
            *end_offset = '\0';

            // mangled name is now in [begin_name, begin_offset) and caller
            // offset in [begin_offset, end_offset). now apply
            // __cxa_demangle():

            int status;
            char* ret = abi::__cxa_demangle(begin_name,
                                            funcname, &funcnamesize, &status);
            if (status == 0) {
                funcname = ret; // use possibly realloc()-ed string
                s2e_debug_print("  %s : %s+%s\n",
                        symbollist[i], funcname, begin_offset);
            }
            else {
                // demangling failed. Output function name as a C function with
                // no arguments.
                s2e_debug_print("  %s : %s()+%s\n",
                        symbollist[i], begin_name, begin_offset);
            }
        }
        else
        {
            // couldn't parse the line? print the whole line.
            s2e_debug_print("  %s\n", symbollist[i]);
        }
    }

    free(funcname);
    free(symbollist);
}
#endif //CONFIG_WIN32

namespace s2e {

using namespace std;

/** A streambuf that writes both to parent streambuf and cerr */
class TeeStreamBuf : public streambuf
{
    deque<streambuf*> m_parentBufs;
public:
    TeeStreamBuf(streambuf* master): m_parentBufs(1, master) {}
    void addParentBuf(streambuf* buf) { m_parentBufs.push_front(buf); }

    streamsize xsputn(const char* s, streamsize n) {
        streamsize res = 0;
        foreach(streambuf* buf, m_parentBufs)
            res = buf->sputn(s, n);
        return res;
    }
    int overflow(int c = EOF ) {
        foreach(streambuf* buf, m_parentBufs)
            buf->sputc(c);
        return 1;
    }
    int sync() {
        int res = 0;
        foreach(streambuf* buf, m_parentBufs)
            res = buf->pubsync();
        return res;
    }
};

class HighlightStreamBuf : public streambuf
{
    streambuf* m_parentBuf;
public:
    HighlightStreamBuf(streambuf* master): m_parentBuf(master) {}

    streamsize xsputn(const char* s, streamsize n) {
        m_parentBuf->sputn("\033[31m", 5);
        streamsize res = m_parentBuf->sputn(s, n);
        m_parentBuf->sputn("\033[0m", 4);
        return res;
    }
    int overflow(int c = EOF) {
        m_parentBuf->sputn("\033[31m", 5);
        m_parentBuf->sputc(c);
        m_parentBuf->sputn("\033[0m", 4);
        return 1;
    }
    int sync() {
        return m_parentBuf->pubsync();
    }
};

S2E::S2E(int argc, char** argv, TCGLLVMContext *tcgLLVMContext,
    const std::string &configFileName, const std::string &outputDirectory,
    int verbose, unsigned s2e_max_processes)
        : m_tcgLLVMContext(tcgLLVMContext)
{
    if (s2e_max_processes < 1) {
        std::cerr << "You must at least allow one process for S2E." << std::endl;
        exit(1);
    }

    if (s2e_max_processes > S2E_MAX_PROCESSES) {
        std::cerr << "S2E can handle at most " << S2E_MAX_PROCESSES << " processes." << std::endl;
        std::cerr << "Please increase the S2E_MAX_PROCESSES constant." << std::endl;
        exit(1);
    }

#ifdef CONFIG_WIN32
    if (s2e_max_processes > 1) {
        std::cerr << "S2E for Windows does not support more than one process" << std::endl;
        exit(1);
    }
#endif

    m_startTimeSeconds = llvm::sys::TimeValue::now().seconds();

    m_maxProcesses = s2e_max_processes;
    m_currentProcessIndex = 0;
    m_currentProcessId = 0;
    S2EShared *shared = m_sync.acquire();
    shared->currentProcessCount = 1;
    shared->lastStateId = 0;
    shared->lastFileId = 1;
    shared->processIds[m_currentProcessId] = m_currentProcessIndex;
    shared->processPids[m_currentProcessId] = getpid();
    m_sync.release();

    /* Open output directory. Do it at the very begining so that
       other init* functions can use it. */
    initOutputDirectory(outputDirectory, verbose, false);

    /* Copy the config file into the output directory */
    {
        ostream *out = openOutputFile("s2e.config.lua");
        ifstream in(configFileName.c_str());
        if(in.good()) {
            (*out) << in.rdbuf();
        }
        delete out;
    }

    /* Save command line arguments */
    {
        ostream *out = openOutputFile("s2e.cmdline");
        for(int i = 0; i < argc; ++i) {
            if(i != 0)
                (*out) << " ";
            (*out) << "'" << argv[i] << "'";
        }
        delete out;
    }

    /* Parse configuration file */
    m_configFile = new s2e::ConfigFile(configFileName);

    /* Initialize KLEE command line options */
    initKleeOptions();

    /* Initialize S2EExecutor */
    initExecutor();

    /* Load and initialize plugins */
    initPlugins();

    /* Init the custom memory allocator */
    //void slab_init();
    //slab_init();
}

void S2E::writeBitCodeToFile()
{
    string execTraceFile = getOutputDirectory();
    execTraceFile += "/";
    execTraceFile += "module.bc";

    ofstream o(execTraceFile.c_str(), ofstream::binary);

    llvm::Module *module = m_tcgLLVMContext->getModule();

    // Output the bitcode file to stdout
    llvm::WriteBitcodeToFile(module, o);
    o.close();
}

S2E::~S2E()
{
    //Delete all the stuff used by the instance
    foreach(Plugin* p, m_activePluginsList)
        delete p;

    //Tell other instances we are dead so they can fork more
    S2EShared *shared = m_sync.acquire();

    assert(shared->processIds[m_currentProcessId] == m_currentProcessIndex);
    shared->processIds[m_currentProcessId] = (unsigned) -1;
    shared->processPids[m_currentProcessId] = (unsigned) -1;
    --shared->currentProcessCount;

    m_sync.release();

    delete m_pluginsFactory;
    delete m_database;

    writeBitCodeToFile();

    // KModule wants to delete the llvm::Module in destroyer.
    // llvm::ModuleProvider wants to delete it too. We have to arbitrate.
    m_tcgLLVMContext->getModuleProvider()->releaseModule();

    //Make sure everything is clean
    m_s2eExecutor->flushTb();

    //This is necessary, as the execution engine uses the module.
    m_tcgLLVMContext->deleteExecutionEngine();

    delete m_s2eExecutor;
    delete m_s2eHandler;

    delete m_configFile;

    delete m_infoFile;

    delete m_warningsFile;
    delete m_warningsStreamBuf;

    delete m_messagesFile;
    delete m_messagesStreamBuf;

    delete m_debugFile;
}

Plugin* S2E::getPlugin(const std::string& name) const
{
    ActivePluginsMap::const_iterator it = m_activePluginsMap.find(name);
    if(it != m_activePluginsMap.end())
        return const_cast<Plugin*>(it->second);
    else
        return NULL;
}

std::string S2E::getOutputFilename(const std::string &fileName)
{
    llvm::sys::Path filePath(m_outputDirectory);
    filePath.appendComponent(fileName);
    return filePath.toString();
}

std::ostream* S2E::openOutputFile(const std::string &fileName)
{
    std::ios::openmode io_mode = std::ios::out | std::ios::trunc
                                  | std::ios::binary;

    std::string path = getOutputFilename(fileName);
    std::ostream *f = new std::ofstream(path.c_str(), io_mode);
    if (!f) {
        std::cerr << "ERROR: out of memory" << std::endl;
        exit(1);
    } else if (!f->good()) {
        std::cerr << "ERROR: can not open file '" << path << "'" << std::endl;
        exit(1);
    }

    return f;
}

void S2E::initOutputDirectory(const string& outputDirectory, int verbose, bool forked)
{
    if (!forked) {
        //In case we create the first S2E process
        if (outputDirectory.empty()) {
            llvm::sys::Path cwd = llvm::sys::Path::GetCurrentDirectory();

            for (int i = 0; ; i++) {
                ostringstream dirName;
                dirName << "s2e-out-" << i;

                llvm::sys::Path dirPath(cwd);
                dirPath.appendComponent(dirName.str());

                if(!dirPath.exists()) {
                    m_outputDirectory = dirPath.toString();
                    break;
                }
            }

        } else {
            m_outputDirectory = outputDirectory;
        }
        m_outputDirectoryBase = m_outputDirectory;
    }else {
        m_outputDirectory = m_outputDirectoryBase;
    }



#ifndef _WIN32
    if (m_maxProcesses > 1) {
        //Create one output directory per child process
        //This prevents child processes to clobber each other's output
        llvm::sys::Path dirPath(m_outputDirectory);

        ostringstream oss;
        oss << m_currentProcessIndex;

        dirPath.appendComponent(oss.str());
        assert(!dirPath.exists());
        m_outputDirectory = dirPath.toString();
    }
#endif

    std::cout << "S2E: output directory = \"" << m_outputDirectory << "\"\n";

    llvm::sys::Path outDir(m_outputDirectory);
    std::string mkdirError;

#ifdef _WIN32
    //XXX: If set to true on Windows, it fails when parent directories exist
    //For now, we assume that only the last component needs to be created
    if (outDir.createDirectoryOnDisk(false, &mkdirError)) {
#else
    if (outDir.createDirectoryOnDisk(true, &mkdirError)) {
#endif
        std::cerr << "Could not create output directory " << outDir.toString() <<
                " error: " << mkdirError << std::endl;
        exit(-1);
    }

#ifndef _WIN32
    if (!forked) {
        llvm::sys::Path s2eLast(".");
        s2eLast.appendComponent("s2e-last");

        if ((unlink(s2eLast.c_str()) < 0) && (errno != ENOENT)) {
            perror("ERRPR: Cannot unlink s2e-last");
            exit(1);
        }

        if (symlink(m_outputDirectoryBase.c_str(), s2eLast.c_str()) < 0) {
            perror("ERROR: Cannot make symlink s2e-last");
            exit(1);
        }
    }
#endif

    ios_base::sync_with_stdio(true);
    cout.setf(ios_base::unitbuf);
    cerr.setf(ios_base::unitbuf);

    m_infoFile = openOutputFile("info");
    m_infoFile->setf(ios_base::unitbuf);

    m_debugFile = openOutputFile("debug.txt");
    m_debugFile->setf(ios_base::unitbuf);
    streambuf* debugFileBuf = m_debugFile->rdbuf();

    m_messagesFile = openOutputFile("messages.txt");
    m_messagesFile->setf(ios_base::unitbuf);
    streambuf* messagesFileBuf = m_messagesFile->rdbuf();

    m_warningsFile = openOutputFile("warnings.txt");
    m_warningsFile->setf(ios_base::unitbuf);
    streambuf* warningsFileBuf = m_warningsFile->rdbuf();

    // Messages appear in messages.txt, debug.txt and on stdout
    m_messagesStreamBuf = new TeeStreamBuf(messagesFileBuf);
    static_cast<TeeStreamBuf*>(m_messagesStreamBuf)->addParentBuf(debugFileBuf);
    if(verbose)
        static_cast<TeeStreamBuf*>(m_messagesStreamBuf)->addParentBuf(cerr.rdbuf());
    m_messagesFile->rdbuf(m_messagesStreamBuf);
    m_messagesFile->setf(ios_base::unitbuf);

    // Warnings appear in warnings.txt, messages.txt, debug.txt
    // and on stderr in red color
    m_warningsStreamBuf = new TeeStreamBuf(warningsFileBuf);
    static_cast<TeeStreamBuf*>(m_warningsStreamBuf)->addParentBuf(messagesFileBuf);
    static_cast<TeeStreamBuf*>(m_warningsStreamBuf)->addParentBuf(debugFileBuf);
    if(verbose)
        static_cast<TeeStreamBuf*>(m_warningsStreamBuf)->addParentBuf(
                          new HighlightStreamBuf(cerr.rdbuf()));
    else
        static_cast<TeeStreamBuf*>(m_warningsStreamBuf)->addParentBuf(cerr.rdbuf());
    m_warningsFile->rdbuf(m_warningsStreamBuf);
    m_warningsFile->setf(ios_base::unitbuf);

    klee::klee_message_stream = m_messagesFile;
    klee::klee_warning_stream = m_warningsFile;

    /* Init the data base */
    m_database = new s2e::Database(m_outputDirectory + "/s2e.db");

}

void S2E::initKleeOptions()
{
    std::vector<std::string> kleeOptions = getConfig()->getStringList("s2e.kleeArgs");
    if(!kleeOptions.empty()) {
        int numArgs = kleeOptions.size() + 1;
        const char **kleeArgv = new const char*[numArgs + 1];

        kleeArgv[0] = "s2e.kleeArgs";
        kleeArgv[numArgs] = 0;

        for(unsigned int i = 0; i < kleeOptions.size(); ++i)
            kleeArgv[i+1] = kleeOptions[i].c_str();

        llvm::cl::ParseCommandLineOptions(numArgs, (char**) kleeArgv);

        delete[] kleeArgv;
    }
}

void S2E::initPlugins()
{
    m_pluginsFactory = new PluginsFactory();

    m_corePlugin = dynamic_cast<CorePlugin*>(
            m_pluginsFactory->createPlugin(this, "CorePlugin"));
    assert(m_corePlugin);

    m_activePluginsList.push_back(m_corePlugin);
    m_activePluginsMap.insert(
            make_pair(m_corePlugin->getPluginInfo()->name, m_corePlugin));
    if(!m_corePlugin->getPluginInfo()->functionName.empty())
        m_activePluginsMap.insert(
            make_pair(m_corePlugin->getPluginInfo()->functionName, m_corePlugin));

    vector<string> pluginNames = getConfig()->getStringList("plugins");

    /* Check and load plugins */
    foreach(const string& pluginName, pluginNames) {
        const PluginInfo* pluginInfo = m_pluginsFactory->getPluginInfo(pluginName);
        if(!pluginInfo) {
            std::cerr << "ERROR: plugin '" << pluginName
                      << "' does not exists in this S2E installation" << std::endl;
            exit(1);
        } else if(getPlugin(pluginInfo->name)) {
            std::cerr << "ERROR: plugin '" << pluginInfo->name
                      << "' was already loaded "
                      << "(is it enabled multiple times ?)" << std::endl;
            exit(1);
        } else if(!pluginInfo->functionName.empty() &&
                    getPlugin(pluginInfo->functionName)) {
            std::cerr << "ERROR: plugin '" << pluginInfo->name
                      << "' with function '" << pluginInfo->functionName
                      << "' can not be loaded because" << std::endl
                      <<  "    this function is already provided by '"
                      << getPlugin(pluginInfo->functionName)->getPluginInfo()->name
                      << "' plugin" << std::endl;
            exit(1);
        } else {
            Plugin* plugin = m_pluginsFactory->createPlugin(this, pluginName);
            assert(plugin);

            m_activePluginsList.push_back(plugin);
            m_activePluginsMap.insert(
                    make_pair(plugin->getPluginInfo()->name, plugin));
            if(!plugin->getPluginInfo()->functionName.empty())
                m_activePluginsMap.insert(
                    make_pair(plugin->getPluginInfo()->functionName, plugin));
        }
    }

    /* Check dependencies */
    foreach(Plugin* p, m_activePluginsList) {
        foreach(const string& name, p->getPluginInfo()->dependencies) {
            if(!getPlugin(name)) {
                std::cerr << "ERROR: plugin '" << p->getPluginInfo()->name
                          << "' depends on plugin '" << name
                          << "' which is not enabled in config" << std::endl;
                exit(1);
            }
        }
    }

    /* Initialize plugins */
    foreach(Plugin* p, m_activePluginsList) {
        p->initialize();
    }
}

void S2E::initExecutor()
{
    m_s2eHandler = new S2EHandler(this);
    S2EExecutor::InterpreterOptions IOpts;
    m_s2eExecutor = new S2EExecutor(this, m_tcgLLVMContext, IOpts, m_s2eHandler);
}

std::ostream& S2E::getStream(std::ostream& stream,
                             const S2EExecutionState* state) const
{
    fflush(stdout);
    fflush(stderr);

    if(state) {
        llvm::sys::TimeValue curTime = llvm::sys::TimeValue::now();
        stream << std::dec << (curTime.seconds() - m_startTimeSeconds) << " ";

        if (m_maxProcesses > 1) {
            stream << std::dec << "[Node " << m_currentProcessIndex <<
                    "/" << m_currentProcessId << " - State " << state->getID() << "] ";
        }else {
            stream << "[State " << std::dec << state->getID() << "] ";
        }
    }
    return stream;
}

void S2E::printf(std::ostream &os, const char *fmt, ...)
{
    va_list vl;
    va_start(vl,fmt);

    char str[512];
    vsnprintf(str, sizeof(str), fmt, vl);
    os << str;
}

void S2E::refreshPlugins()
{
    foreach2(it, m_activePluginsList.begin(), m_activePluginsList.end()) {
        (*it)->refresh();
    }
}

int S2E::fork()
{
#ifdef CONFIG_WIN32
    return -1;
#else

    S2EShared *shared = m_sync.acquire();
    if (shared->currentProcessCount == m_maxProcesses) {
        m_sync.release();
        return -1;
    }

    unsigned newProcessIndex = shared->lastFileId;
    ++shared->lastFileId;
    ++shared->currentProcessCount;

    m_sync.release();

    pid_t pid = ::fork();
    if (pid < 0) {
        //Fork failed

        shared = m_sync.acquire();
        //Do not decrement lastFileId, as other fork may have
        //succeeded while we were handling the failure.

        --shared->currentProcessCount;
        m_sync.release();
        return -1;
    }

    if (pid == 0) {
        //Allocate a free slot in the instance map
        shared = m_sync.acquire();
        unsigned i=0;
        for (i=0; i<m_maxProcesses; ++i) {
            if (shared->processIds[i] == (unsigned)-1) {
                shared->processIds[i] = newProcessIndex;
                shared->processPids[i] = getpid();
                m_currentProcessId = i;
                break;
            }
        }
        assert (i < m_maxProcesses);
        m_sync.release();

        m_currentProcessIndex = newProcessIndex;
        //We are the child process, setup the log files again
        initOutputDirectory(m_outputDirectoryBase, 0, true);
        //Also recreate new statistics files
        m_s2eExecutor->initializeStatistics();
        //And the solver output
        m_s2eExecutor->initializeSolver();

        if (init_timer_alarm()<0) {
            getDebugStream() << "Could not initialize timers" << std::endl;
            exit(-1);
        }
    }

    return pid == 0 ? 1 : 0;
#endif
}

unsigned S2E::fetchAndIncrementStateId()
{
    S2EShared *shared = m_sync.acquire();
    unsigned ret = shared->lastStateId;
    ++shared->lastStateId;
    m_sync.release();
    return ret;
}

unsigned S2E::getCurrentProcessCount()
{
    S2EShared *shared = m_sync.acquire();
    unsigned ret = shared->currentProcessCount;
    m_sync.release();
    return ret;
}

unsigned S2E::getProcessIndexForId(unsigned id)
{
    assert(id < m_maxProcesses);
    S2EShared *shared = m_sync.acquire();
    unsigned ret = shared->processIds[id];
    m_sync.release();
    return ret;
}

bool S2E::checkDeadProcesses()
{
    S2EShared *shared = m_sync.acquire();
    bool ret = false;
    for (unsigned i=0; i<m_maxProcesses; ++i) {
        if (shared->processPids[i] == (unsigned)-1) {
            continue;
        }

        //Check if pid is alive
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "kill -0 %d", shared->processPids[i]);
        int ret = system(buffer);
        if (ret != 0) {
            //Process is dead, we have to decrement everyting
            shared->processIds[i] = (unsigned) -1;
            shared->processPids[i] = (unsigned) -1;
            --shared->currentProcessCount;
            ret = true;
        }
    }

    m_sync.release();
    return ret;
}

} // namespace s2e

/******************************/
/* Functions called from QEMU */

extern "C" {

S2E* g_s2e = NULL;

S2E* s2e_initialize(int argc, char** argv,
            TCGLLVMContext* tcgLLVMContext,
            const char* s2e_config_file,  const char* s2e_output_dir,
            int verbose, unsigned s2e_max_processes)
{
    return new S2E(argc, argv, tcgLLVMContext,
                   s2e_config_file ? s2e_config_file : "",
                   s2e_output_dir  ? s2e_output_dir  : "",
                   verbose, s2e_max_processes);
}

void s2e_close(S2E *s2e)
{
    print_stacktrace();

    delete s2e;
    tcg_llvm_close(tcg_llvm_ctx);
    tcg_llvm_ctx = NULL;
}

void s2e_debug_print(const char *fmtstr, ...)
{
    if (!g_s2e) {
        return;
    }

    va_list vl;
    va_start(vl,fmtstr);

    char str[512];
    vsnprintf(str, sizeof(str), fmtstr, vl);
    g_s2e->getDebugStream() << str;

    va_end(vl);
}

extern "C"
{
void s2e_execute_cmd(const char *cmd)
{
    g_s2e->getConfig()->invokeLuaCommand(cmd);
}
}


} // extern "C"
