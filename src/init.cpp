// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "init.h"
#include "main.h"
#include "chainparams.h"
#include "txdb.h"
#include "rpcserver.h"
#include "rpcclient.h"
#include "net.h"
#include "util.h"
#include "coro.h"

#include "ui_interface.h"
#include "checkpoints.h"
#ifdef ENABLE_WALLET
#include "wallet.h"
#include "walletdb.h"
#endif

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <openssl/crypto.h>


#ifndef _WIN32
#include <signal.h>
#endif


using namespace std;
using namespace boost;

#ifdef ENABLE_WALLET
CWallet* pwalletMain = NULL;
#endif
CClientUIInterface uiInterface;
bool fConfChange;
bool fMinimizeCoinAge;
unsigned int nNodeLifespan;
unsigned int nDerivationMethodIndex;
unsigned int nMinerSleep;
bool fUseFastIndex;
enum Checkpoints::CPMode CheckpointsMode;

bool fRegTest;
bool fTestNet;

std::vector<coro> fiberGroup;

//////////////////////////////////////////////////////////////////////////////
//
// Shutdown
//

//
// Thread management and startup/shutdown:
//
// The network-processing threads are all part of a thread group
// created by AppInit() or the Qt main() function.
//
// A clean exit happens when StartShutdown() or the SIGTERM
// signal handler sets fRequestShutdown, which triggers
// the DetectShutdownThread(), which interrupts the main thread group.
// DetectShutdownThread() then exits, which causes AppInit() to
// continue (it .joins the shutdown thread).
// Shutdown() is then
// called to clean up database connections, and stop other
// threads that should only be stopped after the main network-processing
// threads have exited.
//
// Note that if running -daemon the parent process returns from AppInit2
// before adding any threads to the threadGroup, so .join_all() returns
// immediately and the parent exits from main().
//
// Shutdown for Qt is very similar, only it uses a QTimer to detect
// fRequestShutdown getting set, and then does the normal Qt
// shutdown thing.
//

volatile bool fRequestShutdown = false;

void StartShutdown()
{
    fRequestShutdown = true;
}
bool ShutdownRequested()
{
    return fRequestShutdown;
}

void Shutdown() {
    LogPrintf("Shutdown : In progress...\n");
    mempool.AddTransactionsUpdated(1);
    StopRPCThreads();
#ifdef ENABLE_WALLET
    ShutdownRPCMining();
    if (pwalletMain)
        bitdb.Flush(false);
#endif
    StopNode();

#ifdef ENABLE_WALLET
    if (pwalletMain)
        pwalletMain->SetBestChain(CBlockLocator(pindexBest));
#endif
#ifdef ENABLE_WALLET
    if (pwalletMain)
        bitdb.Flush(true);
#endif
    boost::filesystem::remove(GetPidFile());
    UnregisterAllWallets();
#ifdef ENABLE_WALLET
    delete pwalletMain;
    pwalletMain = NULL;
#endif
    LogPrintf("Shutdown : done\n");
}

//
// Signal handlers are very limited in what they are allowed to do, so:
//
void HandleSIGTERM(int)
{
    fRequestShutdown = true;
}

void HandleSIGHUP(int)
{
    fReopenDebugLog = true;
}

bool static InitError(const std::string &str)
{
    uiInterface.ThreadSafeMessageBox(str, "", CClientUIInterface::MSG_ERROR);
    return false;
}

bool static InitWarning(const std::string &str)
{
    uiInterface.ThreadSafeMessageBox(str, "", CClientUIInterface::MSG_WARNING);
    return true;
}

bool static Bind(const CService &addr, bool fError = true) {
    if (IsLimited(addr))
        return false;
    std::string strError;
    if (!BindListenPort(addr, strError)) {
        if (fError)
            return InitError(strError);
        return false;
    }
    return true;
}

// Core-specific options shared between UI and daemon
std::string HelpMessage()
{
    string strUsage = _("Options:") + "\n";
    strUsage += "  -?                     " + _("This help message") + "\n";
    strUsage += "  -conf=<file>           " + _("Specify configuration file (default: Ember.conf)") + "\n";
    strUsage += "  -pid=<file>            " + _("Specify pid file (default: Emberd.pid)") + "\n";
    strUsage += "  -datadir=<dir>         " + _("Specify data directory") + "\n";
    strUsage += "  -wallet=<dir>          " + _("Specify wallet file (within data directory)") + "\n";
    strUsage += "  -dbcache=<n>           " + _("Set database cache size in megabytes (default: 25)") + "\n";
    strUsage += "  -dblogsize=<n>         " + _("Set database disk log size in megabytes (default: 100)") + "\n";
    strUsage += "  -timeout=<n>           " + _("Specify connection timeout in milliseconds (default: 5000)") + "\n";
    strUsage += "  -proxy=<ip:port>       " + _("Connect through SOCKS5 proxy") + "\n";
    strUsage += "  -tor=<ip:port>         " + _("Use proxy to reach tor hidden services (default: same as -proxy)") + "\n";
    strUsage += "  -dns                   " + _("Allow DNS lookups for -addnode, -seednode and -connect") + "\n";
    strUsage += "  -port=<port>           " + _("Listen for connections on <port> (default: 15714 or testnet: 25714)") + "\n";
    strUsage += "  -maxconnections=<n>    " + _("Maintain at most <n> connections to peers (default: 125)") + "\n";
    strUsage += "  -addnode=<ip>          " + _("Add a node to connect to and attempt to keep the connection open") + "\n";
    strUsage += "  -connect=<ip>          " + _("Connect only to the specified node(s)") + "\n";
    strUsage += "  -seednode=<ip>         " + _("Connect to a node to retrieve peer addresses, and disconnect") + "\n";
    strUsage += "  -externalip=<ip>       " + _("Specify your own public address") + "\n";
    strUsage += "  -onlynet=<net>         " + _("Only connect to nodes in network <net> (IPv4, IPv6 or Tor)") + "\n";
    strUsage += "  -discover              " + _("Discover own IP address (default: 1 when listening and no -externalip)") + "\n";
    strUsage += "  -listen                " + _("Accept connections from outside (default: 1 if no -proxy or -connect)") + "\n";
    strUsage += "  -bind=<addr>           " + _("Bind to given address. Use [host]:port notation for IPv6") + "\n";
    strUsage += "  -dnsseed               " + _("Query for peer addresses via DNS lookup, if low on addresses (default: 1 unless -connect)") + "\n";
    strUsage += "  -forcednsseed          " + _("Always query for peer addresses via DNS lookup (default: 0)") + "\n";
    strUsage += "  -synctime              " + _("Sync time with other nodes. Disable if time on your system is precise e.g. syncing with NTP (default: 1)") + "\n";
    strUsage += "  -cppolicy              " + _("Sync checkpoints policy (default: strict)") + "\n";
    strUsage += "  -banscore=<n>          " + _("Threshold for disconnecting misbehaving peers (default: 100)") + "\n";
    strUsage += "  -bantime=<n>           " + _("Number of seconds to keep misbehaving peers from reconnecting (default: 86400)") + "\n";
    strUsage += "  -maxreceivebuffer=<n>  " + _("Maximum per-connection receive buffer, <n>*1000 bytes (default: 5000)") + "\n";
    strUsage += "  -maxsendbuffer=<n>     " + _("Maximum per-connection send buffer, <n>*1000 bytes (default: 1000)") + "\n";
#ifdef USE_UPNP
#if USE_UPNP
    strUsage += "  -upnp                  " + _("Use UPnP to map the listening port (default: 1 when listening)") + "\n";
#else
    strUsage += "  -upnp                  " + _("Use UPnP to map the listening port (default: 0)") + "\n";
#endif
#endif
    strUsage += "  -paytxfee=<amt>        " + _("Fee per KB to add to transactions you send") + "\n";
    strUsage += "  -mininput=<amt>        " + _("When creating transactions, ignore inputs with value less than this (default: 0.01)") + "\n";
	if (fHaveGUI) {
		strUsage += "  -server                " + _("Accept command line and JSON-RPC commands") + "\n";
	}
    strUsage += "  -daemon                " + _("Run in the background as a daemon and accept commands") + "\n";
    strUsage += "  -testnet               " + _("Use the test network") + "\n";
    strUsage += "  -debug=<category>      " + _("Output debugging information (default: 0, supplying <category> is optional)") + "\n";
    strUsage +=                               _("If <category> is not supplied, output all debugging information.") + "\n";
    strUsage +=                               _("<category> can be:");
    strUsage +=                                 " addrman, alert, db, lock, rand, rpc, selectcoins, mempool, net,"; // Don't translate these and qt below
    strUsage +=                                 " coinage, coinstake, creation, stakemodifier";
    if (fHaveGUI)
    {
        strUsage += ", qt.\n";
    }
    else
    {
        strUsage += ".\n";
    }
    strUsage += "  -logtimestamps         " + _("Prepend debug output with timestamp") + "\n";
    strUsage += "  -shrinkdebugfile       " + _("Shrink debug.log file on client startup (default: 1 when no -debug)") + "\n";
    strUsage += "  -printtoconsole        " + _("Send trace/debug info to console instead of debug.log file") + "\n";
    strUsage += "  -regtest               " + _("Enter regression test mode, which uses a special chain in which blocks can be "
                                                "solved instantly. This is intended for regression testing tools and app development.") + "\n";
    strUsage += "  -rpcuser=<user>        " + _("Username for JSON-RPC connections") + "\n";
    strUsage += "  -rpcpassword=<pw>      " + _("Password for JSON-RPC connections") + "\n";
    strUsage += "  -rpcport=<port>        " + _("Listen for JSON-RPC connections on <port> (default: 15715 or testnet: 25715)") + "\n";
    strUsage += "  -rpcallowip=<ip>       " + _("Allow JSON-RPC connections from specified IP address") + "\n";
    if (!fHaveGUI)
    {
        strUsage += "  -rpcconnect=<ip>       " + _("Send commands to node running on <ip> (default: 127.0.0.1)") + "\n";
        strUsage += "  -rpcwait               " + _("Wait for RPC server to start") + "\n";
    }
    strUsage += "  -rpcthreads=<n>        " + _("Set the number of threads to service RPC calls (default: 4)") + "\n";
    strUsage += "  -blocknotify=<cmd>     " + _("Execute command when the best block changes (%s in cmd is replaced by block hash)") + "\n";
    strUsage += "  -walletnotify=<cmd>    " + _("Execute command when a wallet transaction changes (%s in cmd is replaced by TxID)") + "\n";
    strUsage += "  -confchange            " + _("Require a confirmations for change (default: 0)") + "\n";
    strUsage += "  -minimizecoinage       " + _("Minimize weight consumption (experimental) (default: 0)") + "\n";
    strUsage += "  -alertnotify=<cmd>     " + _("Execute command when a relevant alert is received (%s in cmd is replaced by message)") + "\n";
    strUsage += "  -upgradewallet         " + _("Upgrade wallet to latest format") + "\n";
    strUsage += "  -keypool=<n>           " + _("Set key pool size to <n> (default: 100)") + "\n";
    strUsage += "  -rescan                " + _("Rescan the block chain for missing wallet transactions") + "\n";
    strUsage += "  -salvagewallet         " + _("Attempt to recover private keys from a corrupt wallet.dat") + "\n";
    strUsage += "  -checkblocks=<n>       " + _("How many blocks to check at startup (default: 500, 0 = all)") + "\n";
    strUsage += "  -checklevel=<n>        " + _("How thorough the block verification is (0-6, default: 1)") + "\n";
    strUsage += "  -loadblock=<file>      " + _("Imports blocks from external blk000?.dat file") + "\n";
    strUsage += "  -maxorphanblocks=<n>   " + strprintf(_("Keep at most <n> unconnectable blocks in memory (default: %u)"), DEFAULT_MAX_ORPHAN_BLOCKS) + "\n";

    strUsage += "\n" + _("Block creation options:") + "\n";
    strUsage += "  -blockminsize=<n>      "   + _("Set minimum block size in bytes (default: 0)") + "\n";
    strUsage += "  -blockmaxsize=<n>      "   + _("Set maximum block size in bytes (default: 250000)") + "\n";
    strUsage += "  -blockprioritysize=<n> "   + _("Set maximum size of high-priority/low-fee transactions in bytes (default: 27000)") + "\n";

    strUsage += "\n" + _("SSL options: (see the Bitcoin Wiki for SSL setup instructions)") + "\n";
    strUsage += "  -rpcssl                                  " + _("Use OpenSSL (https) for JSON-RPC connections") + "\n";
    strUsage += "  -rpcsslcertificatechainfile=<file.cert>  " + _("Server certificate file (default: server.cert)") + "\n";
    strUsage += "  -rpcsslprivatekeyfile=<file.pem>         " + _("Server private key (default: server.pem)") + "\n";
    strUsage += "  -rpcsslciphers=<ciphers>                 " + _("Acceptable ciphers (default: TLSv1.2+HIGH:TLSv1+HIGH:!SSLv2:!aNULL:!eNULL:!3DES:@STRENGTH)") + "\n";

    return strUsage;
}

/** Sanity checks
 *  Ensure that Bitcoin is running in a usable environment with all
 *  necessary library support.
 */
bool InitSanityCheck(void)
{
    if(!ECC_InitSanityCheck()) {
        InitError("OpenSSL appears to lack support for elliptic curve cryptography. For more "
                  "information, visit https://en.bitcoin.it/wiki/OpenSSL_and_EC_Libraries");
        return false;
    }

    // TODO: remaining sanity checks, see #4081

    return true;
}

/** Initialize bitcoin.
*  @pre Parameters should be parsed and config file should be read.
*/
bool AppInit(int argc, char* argv[]) {
	try {
		// If Qt is used, parameters/bitcoin.conf are parsed in qt/bitcoin.cpp's main()
		ParseParameters(argc, argv);
		if (!boost::filesystem::is_directory(GetDataDir(false))) {
			fprintf(stderr, "Error: Specified directory does not exist\n");
			Shutdown();
		}
		ReadConfigFile(mapArgs, mapMultiArgs);

		if (mapArgs.count("-?") || mapArgs.count("--help")) {
			// First part of help message is specific to bitcoind / RPC client
			std::string strUsage = _("Ember version") + " " + FormatFullVersion() + "\n\n" +
				_("Usage:") + "\n" +
				"  Emberd [options]                     " + "\n" +
				"  Emberd [options] <command> [params]  " + _("Send command to -server or Emberd") + "\n" +
				"  Emberd [options] help                " + _("List commands") + "\n" +
				"  Emberd [options] help <command>      " + _("Get help for a command") + "\n";

			strUsage += "\n" + HelpMessage();

			fprintf(stdout, "%s", strUsage.c_str());
			return false;
		}

		// Command-line RPC
		for (int i = 1; i < argc; i++) {
			if (!IsSwitchChar(argv[i][0]) && !boost::algorithm::istarts_with(argv[i], "Ember:")) {
				fCommandLine = true;
			}
		}

		if (fCommandLine) {
			bool fRegTest = GetBoolArg("-regtest", false);
			bool fTestNet = GetBoolArg("-testnet", false);

			if (fTestNet && fRegTest) {
				fprintf(stderr, "Error: invalid combination of -regtest and -testnet.\n");
			}

			if (fRegTest) {
				SelectParams(CChainParams::REGTEST);
			}
			else if (fTestNet) {
				SelectParams(CChainParams::TESTNET);
			}
			else {
				SelectParams(CChainParams::MAIN);
			}

			int ret = CommandLineRPC(argc, argv);
			exit(ret);
		}
#ifndef _WIN32
		fDaemon = GetBoolArg("-daemon", false);
		if (fDaemon)
		{
			// Daemonize
			pid_t pid = fork();
			if (pid < 0) {
				fprintf(stderr, "Error: fork() returned %d errno %d\n", pid, errno);
				return false;
			}
			if (pid > 0) { // Parent process, pid is child process id
				CreatePidFile(GetPidFile(), pid);
				return true;
			}
			// Child process falls through to rest of initialization

			pid_t sid = setsid();
			if (sid < 0) {
				fprintf(stderr, "Error: setsid() returned %d errno %d\n", sid, errno);
			}
		}
#endif

			// ********************************************************* Step 1: setup
#ifdef _MSC_VER
			// Turn off Microsoft heap dump noise
			_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
			_CrtSetReportFile(_CRT_WARN, CreateFileA("NUL", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, 0));
#endif
#if _MSC_VER >= 1400
			// Disable confusing "helpful" text message on abort, Ctrl-C
			_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#ifdef _WIN32
			// Enable Data Execution Prevention (DEP)
			// Minimum supported OS versions: WinXP SP3, WinVista >= SP1, Win Server 2008
			// A failure is non-critical and needs no further attention!
#ifndef PROCESS_DEP_ENABLE
			// We define this here, because GCCs winbase.h limits this to _WIN32_WINNT >= 0x0601 (Windows 7),
			// which is not correct. Can be removed, when GCCs winbase.h is fixed!
#define PROCESS_DEP_ENABLE 0x00000001
#endif
			typedef BOOL(WINAPI *PSETPROCDEPPOL)(DWORD);
			PSETPROCDEPPOL setProcDEPPol = (PSETPROCDEPPOL)GetProcAddress(GetModuleHandleA("Kernel32.dll"), "SetProcessDEPPolicy");
			if (setProcDEPPol != NULL) setProcDEPPol(PROCESS_DEP_ENABLE);
#endif
#ifndef _WIN32
			umask(077);

			// Clean shutdown on SIGTERM
			struct sigaction sa;
			sa.sa_handler = HandleSIGTERM;
			sigemptyset(&sa.sa_mask);
			sa.sa_flags = 0;
			sigaction(SIGTERM, &sa, NULL);
			sigaction(SIGINT, &sa, NULL);

			// Reopen debug.log on SIGHUP
			struct sigaction sa_hup;
			sa_hup.sa_handler = HandleSIGHUP;
			sigemptyset(&sa_hup.sa_mask);
			sa_hup.sa_flags = 0;
			sigaction(SIGHUP, &sa_hup, NULL);
#endif

			// ********************************************************* Step 2: parameter interactions

			nNodeLifespan = GetArg("-addrlifespan", 7);
			fUseFastIndex = GetBoolArg("-fastindex", true);
			nMinerSleep = GetArg("-minersleep", 500);

			CheckpointsMode = Checkpoints::STRICT;
			std::string strCpMode = GetArg("-cppolicy", "strict");

			if (strCpMode == "strict")
				CheckpointsMode = Checkpoints::STRICT;

			if (strCpMode == "advisory")
				CheckpointsMode = Checkpoints::ADVISORY;

			if (strCpMode == "permissive")
				CheckpointsMode = Checkpoints::PERMISSIVE;

			nDerivationMethodIndex = 0;


			fRegTest = GetBoolArg("-regtest", false);
			fTestNet = GetBoolArg("-testnet", false);

			if (fTestNet && fRegTest) {
				InitError("Invalid combination of -testnet and -regtest.");
				goto error;
			}

			if (fRegTest) {
				SelectParams(CChainParams::REGTEST);
			}
			else if (fTestNet) {
				SelectParams(CChainParams::TESTNET);
				mapMultiArgs["-addnode"].push_back("127.0.0.1:15724");
				mapMultiArgs["-addnode"].push_back("127.0.0.1:15734");
			}
			else {
				SelectParams(CChainParams::MAIN);
				mapMultiArgs["-addnode"].push_back("107.161.30.232:10024");
			}

			if (mapArgs.count("-bind")) {
				// when specifying an explicit binding address, you want to listen on it
				// even when -connect or -proxy is specified
				if (SoftSetBoolArg("-listen", true))
					LogPrintf("AppInit : parameter interaction: -bind set -> setting -listen=1\n");
			}

			if (mapArgs.count("-connect") && mapMultiArgs["-connect"].size() > 0) {
				// when only connecting to trusted nodes, do not seed via DNS, or listen by default
				if (SoftSetBoolArg("-dnsseed", false))
					LogPrintf("AppInit : parameter interaction: -connect set -> setting -dnsseed=0\n");
				if (SoftSetBoolArg("-listen", false))
					LogPrintf("AppInit : parameter interaction: -connect set -> setting -listen=0\n");
			}

			if (mapArgs.count("-proxy")) {
				// to protect privacy, do not listen by default if a default proxy server is specified
				if (SoftSetBoolArg("-listen", false))
					LogPrintf("AppInit : parameter interaction: -proxy set -> setting -listen=0\n");
				// to protect privacy, do not discover addresses by default
				if (SoftSetBoolArg("-discover", false))
					LogPrintf("AppInit : parameter interaction: -proxy set -> setting -discover=0\n");
			}

			if (!GetBoolArg("-listen", true)) {
				// do not map ports or try to retrieve public IP when not listening (pointless)
				if (SoftSetBoolArg("-upnp", false))
					LogPrintf("AppInit : parameter interaction: -listen=0 -> setting -upnp=0\n");
				if (SoftSetBoolArg("-discover", false))
					LogPrintf("AppInit : parameter interaction: -listen=0 -> setting -discover=0\n");
			}

			if (mapArgs.count("-externalip")) {
				// if an explicit public IP is specified, do not try to find others
				if (SoftSetBoolArg("-discover", false))
					LogPrintf("AppInit : parameter interaction: -externalip set -> setting -discover=0\n");
			}

			if (GetBoolArg("-salvagewallet", false)) {
				// Rewrite just private keys: rescan to find transactions
				if (SoftSetBoolArg("-rescan", true))
					LogPrintf("AppInit : parameter interaction: -salvagewallet=1 -> setting -rescan=1\n");
			}
			ReadConfigFile(mapArgs, mapMultiArgs);

			// ********************************************************* Step 3: parameter-to-internal-flags

			fDebug = !mapMultiArgs["-debug"].empty();
			// Special-case: if -debug=0/-nodebug is set, turn off debugging messages
			const vector<string>& categories = mapMultiArgs["-debug"];
			if (GetBoolArg("-nodebug", false) || find(categories.begin(), categories.end(), string("0")) != categories.end())
				fDebug = false;

			// Check for -socks - as this is a privacy risk to continue, exit here
			if (mapArgs.count("-socks")) {
				InitError(_("Error: Unsupported argument -socks found. Setting SOCKS version isn't possible anymore, only SOCKS5 proxies are supported."));
				goto error;
			}

			if (fDaemon)
				fServer = true;
			else
				fServer = GetBoolArg("-server", false);

			/* force fServer when running without GUI */
			if (!fHaveGUI)
				fServer = true;
			fPrintToConsole = GetBoolArg("-printtoconsole", false);
			fLogTimestamps = GetBoolArg("-logtimestamps", false);
#ifdef ENABLE_WALLET
			bool fDisableWallet = GetBoolArg("-disablewallet", false);
#endif

			if (mapArgs.count("-timeout"))
			{
				int nNewTimeout = GetArg("-timeout", 120000);
				if (nNewTimeout > 0 && nNewTimeout < 600000)
					nConnectTimeout = nNewTimeout;
			}

#ifdef ENABLE_WALLET
			if (mapArgs.count("-paytxfee"))
			{
				if (!ParseMoney(mapArgs["-paytxfee"], nTransactionFee)) {
					InitError(strprintf(_("Invalid amount for -paytxfee=<amount>: '%s'"), mapArgs["-paytxfee"]));
					goto error;
				}
				if (nTransactionFee > 0.25 * COIN) {
					InitWarning(_("Warning: -paytxfee is set very high! This is the transaction fee you will pay if you send a transaction."));
				}
			}
#endif

			fConfChange = GetBoolArg("-confchange", false);
			fMinimizeCoinAge = GetBoolArg("-minimizecoinage", false);

#ifdef ENABLE_WALLET
			if (mapArgs.count("-mininput"))
			{
				if (!ParseMoney(mapArgs["-mininput"], nMinimumInputValue)) {
					InitError(strprintf(_("Invalid amount for -mininput=<amount>: '%s'"), mapArgs["-mininput"]));
					goto error;
				}
			}
#endif

			// ********************************************************* Step 4: application initialization: dir lock, daemonize, pidfile, debug log

			// Sanity check
			if (!InitSanityCheck()) {
				InitError(_("Initialization sanity check failed. Ember is shutting down."));
				goto error;
			}

			std::string strDataDir = GetDataDir().string();
#ifdef ENABLE_WALLET
			std::string strWalletFileName = GetArg("-wallet", "wallet.dat");

			// strWalletFileName must be a plain filename without a directory
			if (strWalletFileName != boost::filesystem::basename(strWalletFileName) + boost::filesystem::extension(strWalletFileName)) {
				InitError(strprintf(_("Wallet %s resides outside data directory %s."), strWalletFileName, strDataDir));
				goto error;
			}
#endif
			// Make sure only a single Bitcoin process is using the data directory.
			boost::filesystem::path pathLockFile = GetDataDir() / ".lock";
			FILE* file = fopen(pathLockFile.string().c_str(), "a"); // empty lock file; created if it doesn't exist.
			if (file) fclose(file);
			static boost::interprocess::file_lock lock(pathLockFile.string().c_str());
			if (!lock.try_lock()) {
				InitError(strprintf(_("Cannot obtain a lock on data directory %s. Ember is probably already running."), strDataDir));
				goto error;
			}

			if (GetBoolArg("-shrinkdebugfile", !fDebug))
				ShrinkDebugFile();
			LogPrintf("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
			LogPrintf("Ember version %s (%s)\n", FormatFullVersion(), CLIENT_DATE);
			LogPrintf("Using LibreSSL version %s\n", SSLeay_version(SSLEAY_VERSION));
			if (!fLogTimestamps)
				LogPrintf("Startup time: %s\n", DateTimeStrFormat("%x %H:%M:%S", GetTime()));
			LogPrintf("Default data directory %s\n", GetDefaultDataDir().string());
			LogPrintf("Used data directory %s\n", strDataDir);
			std::ostringstream strErrors;

			if (fDaemon)
				fprintf(stdout, "Ember server starting\n");

			int64_t nStart;

			// ********************************************************* Step 5: verify database integrity
#ifdef ENABLE_WALLET
			if (!fDisableWallet) {
				uiInterface.InitMessage(_("Verifying database integrity..."));

				if (!bitdb.Open(GetDataDir()))
				{
					// try moving the database env out of the way
					boost::filesystem::path pathDatabase = GetDataDir() / "database";
					boost::filesystem::path pathDatabaseBak = GetDataDir() / strprintf("database.%d.bak", GetTime());
					try {
						boost::filesystem::rename(pathDatabase, pathDatabaseBak);
						LogPrintf("Moved old %s to %s. Retrying.\n", pathDatabase.string(), pathDatabaseBak.string());
					}
					catch (boost::filesystem::filesystem_error &error) {
						// failure is ok (well, not really, but it's not worse than what we started with)
					}

					// try again
					if (!bitdb.Open(GetDataDir())) {
						// if it still fails, it probably means we can't even create the database env
						string msg = strprintf(_("Error initializing wallet database environment %s!"), strDataDir);
						InitError(msg);
						goto error;
					}
				}

				if (GetBoolArg("-salvagewallet", false)) {
					// Recover readable keypairs:
					if (!CWalletDB::Recover(bitdb, strWalletFileName, true)) {
						goto error;
					}
				}

				if (filesystem::exists(GetDataDir() / strWalletFileName)) {
					/*            CDBEnv::VerifyResult r = bitdb.Verify(strWalletFileName, CWalletDB::Recover);
					if (r == CDBEnv::RECOVER_OK)
					{
					string msg = strprintf(_("Warning: wallet.dat corrupt, data salvaged!"
					" Original wallet.dat saved as wallet.{timestamp}.bak in %s; if"
					" your balance or transactions are incorrect you should"
					" restore from a backup."), strDataDir);
					InitWarning(msg);
					}
					if (r == CDBEnv::RECOVER_FAIL)
					return InitError(_("wallet.dat corrupt, salvage failed"));
					*/
				}
			} // (!fDisableWallet)
#endif // ENABLE_WALLET
			  // ********************************************************* Step 6: network initialization

			RegisterNodeSignals(GetNodeSignals());

			if (mapArgs.count("-onlynet")) {
				std::set<enum Network> nets;
				BOOST_FOREACH(std::string snet, mapMultiArgs["-onlynet"]) {
					enum Network net = ParseNetwork(snet);
					if (net == NET_UNROUTABLE) {
						InitError(strprintf(_("Unknown network specified in -onlynet: '%s'"), snet));
						goto error;
					}
					nets.insert(net);
				}
				for (int n = 0; n < NET_MAX; n++) {
					enum Network net = (enum Network)n;
					if (!nets.count(net))
						SetLimited(net);
				}
			}

			CService addrProxy;
			bool fProxy = false;
			if (mapArgs.count("-proxy")) {
				addrProxy = CService(mapArgs["-proxy"], 9050);
				if (!addrProxy.IsValid()) {
					InitError(strprintf(_("Invalid -proxy address: '%s'"), mapArgs["-proxy"]));
					goto error;
				}

				if (!IsLimited(NET_IPV4))
					SetProxy(NET_IPV4, addrProxy);
				if (!IsLimited(NET_IPV6))
					SetProxy(NET_IPV6, addrProxy);
				SetNameProxy(addrProxy);
				fProxy = true;
			}

			// -tor can override normal proxy, -notor disables tor entirely
			if (!(mapArgs.count("-tor") && mapArgs["-tor"] == "0") && (fProxy || mapArgs.count("-tor"))) {
				CService addrOnion;
				if (!mapArgs.count("-tor"))
					addrOnion = addrProxy;
				else
					addrOnion = CService(mapArgs["-tor"], 9050);
				if (!addrOnion.IsValid()) {
					InitError(strprintf(_("Invalid -tor address: '%s'"), mapArgs["-tor"]));
					goto error;
				}
				SetProxy(NET_TOR, addrOnion);
				SetReachable(NET_TOR);
			}

			// see Step 2: parameter interactions for more information about these
			fNoListen = !GetBoolArg("-listen", true);
			fDiscover = GetBoolArg("-discover", true);
			fNameLookup = GetBoolArg("-dns", true);

			bool fBound = false;
			if (!fNoListen) {
				std::string strError;
				if (mapArgs.count("-bind")) {
					BOOST_FOREACH(std::string strBind, mapMultiArgs["-bind"]) {
						CService addrBind;
						if (!Lookup(strBind.c_str(), addrBind, GetListenPort(), false)) {
							InitError(strprintf(_("Cannot resolve -bind address: '%s'"), strBind));
							goto error;
						}
						fBound |= Bind(addrBind);
					}
				}
				else {
					struct in_addr inaddr_any;
					inaddr_any.s_addr = INADDR_ANY;
					if (!IsLimited(NET_IPV6))
						fBound |= Bind(CService(in6addr_any, GetListenPort()), false);
					if (!IsLimited(NET_IPV4))
						fBound |= Bind(CService(inaddr_any, GetListenPort()), !fBound);
				}
				if (!fBound) {
					InitError(_("Failed to listen on any port. Use -listen=0 if you want this."));
					goto error;
				}
			}

			if (mapArgs.count("-externalip")) {
				BOOST_FOREACH(string strAddr, mapMultiArgs["-externalip"]) {
					CService addrLocal(strAddr, GetListenPort(), fNameLookup);
					if (!addrLocal.IsValid()) {
						InitError(strprintf(_("Cannot resolve -externalip address: '%s'"), strAddr));
						goto error;
					}
					AddLocal(CService(strAddr, GetListenPort(), fNameLookup), LOCAL_MANUAL);
				}
			}

#ifdef ENABLE_WALLET
			if (mapArgs.count("-reservebalance")) { // ppcoin: reserve balance amount
				if (!ParseMoney(mapArgs["-reservebalance"], nReserveBalance)) {
					InitError(_("Invalid amount for -reservebalance=<amount>"));
					goto error;
				}
			}
#endif

			if (mapArgs.count("-checkpointkey")) { // ppcoin: checkpoint master priv key
				if (!Checkpoints::SetCheckpointPrivKey(GetArg("-checkpointkey", ""))) {
					InitError(_("Unable to sign checkpoint, wrong checkpointkey?\n"));
				}
			}

			BOOST_FOREACH(string strDest, mapMultiArgs["-seednode"])
				AddOneShot(strDest);

			// ********************************************************* Step 7: load blockchain

			if (GetBoolArg("-loadblockindextest", false)) {
				CTxDB txdb("r");
				txdb.LoadBlockIndex();
				PrintBlockTree();
				goto error;
			}

			uiInterface.InitMessage(_("Loading block index..."));

			nStart = GetTimeMillis();
			if (!LoadBlockIndex()) {
				InitError(_("Error loading block database"));
				goto error;
			}


			// as LoadBlockIndex can take several minutes, it's possible the user
			// requested to kill bitcoin-qt during the last operation. If so, exit.
			// As the program has not fully started yet, Shutdown() is possibly overkill.
			if (fRequestShutdown) {
				LogPrintf("Shutdown requested. Exiting.\n");
				goto error;
			}
			LogPrintf(" block index %15dms\n", GetTimeMillis() - nStart);

			if (GetBoolArg("-printblockindex", false) || GetBoolArg("-printblocktree", false)) {
				PrintBlockTree();
				goto error;
			}

			if (mapArgs.count("-printblock")) {
				string strMatch = mapArgs["-printblock"];
				int nFound = 0;
				for (map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
				{
					uint256 hash = (*mi).first;
					if (strncmp(hash.ToString().c_str(), strMatch.c_str(), strMatch.size()) == 0)
					{
						CBlockIndex* pindex = (*mi).second;
						CBlock block;
						block.ReadFromDisk(pindex);
						block.BuildMerkleTree();
						LogPrintf("%s\n", block.ToString());
						nFound++;
					}
				}
				if (nFound == 0) {
					LogPrintf("No blocks matching %s were found\n", strMatch);
				}
				goto error;
			}

			// ********************************************************* Step 8: load wallet
#ifdef ENABLE_WALLET
			if (fDisableWallet) {
				pwalletMain = NULL;
				LogPrintf("Wallet disabled!\n");
			} else {
				uiInterface.InitMessage(_("Loading wallet..."));

				nStart = GetTimeMillis();
				bool fFirstRun = true;
				pwalletMain = new CWallet(strWalletFileName);
				DBErrors nLoadWalletRet = pwalletMain->LoadWallet(fFirstRun);
				if (nLoadWalletRet != DB_LOAD_OK) {
					if (nLoadWalletRet == DB_CORRUPT) {
						strErrors << _("Error loading wallet.dat: Wallet corrupted") << "\n";
					} else if (nLoadWalletRet == DB_NONCRITICAL_ERROR) {
						string msg(_("Warning: error reading wallet.dat! All keys read correctly, but transaction data"
							" or address book entries might be missing or incorrect."));
						InitWarning(msg);
					} else if (nLoadWalletRet == DB_TOO_NEW) {
						strErrors << _("Error loading wallet.dat: Wallet requires newer version of Ember") << "\n";
					} else if (nLoadWalletRet == DB_NEED_REWRITE) {
						strErrors << _("Wallet needed to be rewritten: restart Ember to complete") << "\n";
						LogPrintf("%s", strErrors.str());
						return InitError(strErrors.str());
					} else {
						strErrors << _("Error loading wallet.dat") << "\n";
					}
				}

				if (GetBoolArg("-upgradewallet", fFirstRun))
				{
					int nMaxVersion = GetArg("-upgradewallet", 0);
					if (nMaxVersion == 0) { // the -upgradewallet without argument case
						LogPrintf("Performing wallet upgrade to %i\n", FEATURE_LATEST);
						nMaxVersion = CLIENT_VERSION;
						pwalletMain->SetMinVersion(FEATURE_LATEST); // permanently upgrade the wallet immediately
					} else {
						LogPrintf("Allowing wallet upgrade up to %i\n", nMaxVersion);
					}
					if (nMaxVersion < pwalletMain->GetVersion()) {
						strErrors << _("Cannot downgrade wallet") << "\n";
					}
					pwalletMain->SetMaxVersion(nMaxVersion);
				}

				if (fFirstRun) {
					// Create new keyUser and set as default key
					RandAddSeedPerfmon();

					CPubKey newDefaultKey;
					if (pwalletMain->GetKeyFromPool(newDefaultKey)) {
						pwalletMain->SetDefaultKey(newDefaultKey);
						if (!pwalletMain->SetAddressBookName(pwalletMain->vchDefaultKey.GetID(), ""))
							strErrors << _("Cannot write default address") << "\n";
					}

					pwalletMain->SetBestChain(CBlockLocator(pindexBest));
				}

				LogPrintf("%s", strErrors.str());
				LogPrintf(" wallet      %15dms\n", GetTimeMillis() - nStart);

				RegisterWallet(pwalletMain);

				CBlockIndex *pindexRescan = pindexBest;
				if (GetBoolArg("-rescan", false)) {
					pindexRescan = pindexGenesisBlock;
				}
				else {
					CWalletDB walletdb(strWalletFileName);
					CBlockLocator locator;
					if (walletdb.ReadBestBlock(locator))
						pindexRescan = locator.GetBlockIndex();
					else
						pindexRescan = pindexGenesisBlock;
				}
				if (pindexBest != pindexRescan && pindexBest && pindexRescan && pindexBest->nHeight > pindexRescan->nHeight) {
					uiInterface.InitMessage(_("Rescanning..."));
					LogPrintf("Rescanning last %i blocks (from block %i)...\n", pindexBest->nHeight - pindexRescan->nHeight, pindexRescan->nHeight);
					nStart = GetTimeMillis();
					pwalletMain->ScanForWalletTransactions(pindexRescan, true);
					LogPrintf(" rescan      %15dms\n", GetTimeMillis() - nStart);
					pwalletMain->SetBestChain(CBlockLocator(pindexBest));
					nWalletDBUpdated++;
				}
			} // (!fDisableWallet)
#else // ENABLE_WALLET
			LogPrintf("No wallet compiled in!\n");
#endif // !ENABLE_WALLET
			// ********************************************************* Step 9: load peers

			uiInterface.InitMessage(_("Loading addresses..."));

			nStart = GetTimeMillis();

			{
				CAddrDB adb;
				if (!adb.Read(addrman)) {
					LogPrintf("Invalid or missing peers.dat; recreating\n");
				}

				LogPrintf("Loaded %i addresses from peers.dat  %dms\n",
					addrman.size(), GetTimeMillis() - nStart);
			}

			// ********************************************************* Step 10: start node

			if (!CheckDiskSpace()) {
				goto error;
			}

			if (!strErrors.str().empty()) {
				InitError(strErrors.str());
				goto error;
			}

			RandAddSeedPerfmon();

			//// debug print
			LogPrintf("mapBlockIndex.size() = %u\n", mapBlockIndex.size());
			LogPrintf("nBestHeight = %d\n", nBestHeight);
#ifdef ENABLE_WALLET
			LogPrintf("setKeyPool.size() = %u\n", pwalletMain ? pwalletMain->setKeyPool.size() : 0);
			LogPrintf("mapWallet.size() = %u\n", pwalletMain ? pwalletMain->mapWallet.size() : 0);
			LogPrintf("mapAddressBook.size() = %u\n", pwalletMain ? pwalletMain->mapAddressBook.size() : 0);
#endif

			StartNode(fiberGroup);
#ifdef ENABLE_WALLET
			// InitRPCMining is needed here so getwork/getblocktemplate in the GUI debug console works properly.
			InitRPCMining();
#endif
			if (fServer) {
				StartRPCThreads();
			}

#ifdef ENABLE_WALLET
			// Mine proof-of-stake blocks in the background
			if (!GetBoolArg("-staking", true)) {
				LogPrintf("Staking disabled\n");
			}
			else if (pwalletMain) {
				coro c;
				c.coro = StakeMiner;
				c.time = 0;
				fiberGroup.push_back(c);
			}
#endif

			// ********************************************************* Step 11: finished

			uiInterface.InitMessage(_("Done loading"));

#ifdef ENABLE_WALLET
			if (pwalletMain) {
				pwalletMain->ReacceptWalletTransactions(); // Add wallet transactions that aren't already in a block to mapTransactions
				fiberGroup.push_back(new coro_context());
				coro_create(fiberGroup.back(), (coro_func)&FlushWalletDB, &(pwalletMain->strWalletFile), NULL, 0); // Run a fiber to flush wallet periodically
			}
#endif
	} catch (std::exception& e) {
		PrintException(&e, "AppInit()");
	} catch (...) {
		PrintException(NULL, "AppInit()");
	}
	coro_context root = coro_context();
	coro_create(&root, 0, 0, 0, 0);

	using std::chrono::steady_clock;
	auto start = steady_clock::now();
	/* Do stuff */
	auto end = steady_clock::now();
	double elapsedSeconds = ((end - start).count()) * steady_clock::period::num / static_cast<double>(steady_clock::period::den);

	while (!fiberGroup.empty()) {
		for (int i = 0; i < fiberGroup.size(); i++) {
			t = coro_transfer(&root, fiberGroup[i]);
		}
	}
	Shutdown();
	return true;
error:
	Shutdown();
	return false;
}

extern void noui_connect();
int main(int argc, char* argv[]) {
	bool fRet = false;
	fHaveGUI = false;

	// Connect bitcoind signal handlers
	noui_connect();

	fRet = AppInit(argc, argv);

	if (fRet && fDaemon)
		return 0;

	return (fRet ? 0 : 1);
}



