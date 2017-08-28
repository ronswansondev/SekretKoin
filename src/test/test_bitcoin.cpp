// Copyright (c) 2011-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#define BOOST_TEST_MODULE Bitcoin Test Suite

#include "test_bitcoin.h"

#include "chainparams.h"
#include "config.h"
#include "consensus/consensus.h"
#include "consensus/validation.h"
#include "key.h"
#include "miner.h"
#include "net_processing.h"
#include "pubkey.h"
#include "random.h"
#include "rpc/register.h"
#include "rpc/server.h"
#include "script/sigcache.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "validation.h"

#include "test/testutil.h"

#include <memory>

#include <boost/filesystem.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/thread.hpp>

std::unique_ptr<CConnman> g_connman;
FastRandomContext insecure_rand_ctx(true);

extern bool fPrintToConsole;
extern void noui_connect();

BasicTestingSetup::BasicTestingSetup(const std::string &chainName) {
    ECC_Start();
    SetupEnvironment();
    SetupNetworking();
    InitSignatureCache();
    // Don't want to write to debug.log file.
    fPrintToDebugLog = false;
    fCheckBlockIndex = true;
    SelectParams(chainName);
    noui_connect();

    // Set config parameters to default.
    GlobalConfig config;
    config.SetUAHFStartTime(DEFAULT_UAHF_START_TIME);
    config.SetMaxBlockSize(DEFAULT_MAX_BLOCK_SIZE);
}

BasicTestingSetup::~BasicTestingSetup() {
    ECC_Stop();
    g_connman.reset();
}

TestingSetup::TestingSetup(const std::string &chainName)
    : BasicTestingSetup(chainName) {

    // Ideally we'd move all the RPC tests to the functional testing framework
    // instead of unit tests, but for now we need these here.
    const Config &config = GetConfig();
    RegisterAllRPCCommands(tableRPC);
    ClearDatadirCache();
    pathTemp = GetTempPath() / strprintf("test_bitcoin_%lu_%i",
                                         (unsigned long)GetTime(),
                                         (int)(GetRand(100000)));
    boost::filesystem::create_directories(pathTemp);
    ForceSetArg("-datadir", pathTemp.string());
    mempool.setSanityCheck(1.0);
    pblocktree = new CBlockTreeDB(1 << 20, true);
    pcoinsdbview = new CCoinsViewDB(1 << 23, true);
    pcoinsTip = new CCoinsViewCache(pcoinsdbview);
    InitBlockIndex(config);
    {
        CValidationState state;
        bool ok = ActivateBestChain(config, state);
        BOOST_CHECK(ok);
    }
    nScriptCheckThreads = 3;
    for (int i = 0; i < nScriptCheckThreads - 1; i++) {
        threadGroup.create_thread(&ThreadScriptCheck);
    }

    // Deterministic randomness for tests.
    g_connman = std::unique_ptr<CConnman>(new CConnman(0x1337, 0x1337));
    connman = g_connman.get();
    RegisterNodeSignals(GetNodeSignals());
}

TestingSetup::~TestingSetup() {
    UnregisterNodeSignals(GetNodeSignals());
    threadGroup.interrupt_all();
    threadGroup.join_all();
    UnloadBlockIndex();
    delete pcoinsTip;
    delete pcoinsdbview;
    delete pblocktree;
    boost::filesystem::remove_all(pathTemp);
}

TestChain100Setup::TestChain100Setup()
    : TestingSetup(CBaseChainParams::REGTEST) {
    // Generate a 100-block chain:
    coinbaseKey.MakeNewKey(true);
    CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey())
                                     << OP_CHECKSIG;
    for (int i = 0; i < COINBASE_MATURITY; i++) {
        std::vector<CMutableTransaction> noTxns;
        CBlock b = CreateAndProcessBlock(noTxns, scriptPubKey);
        coinbaseTxns.push_back(*b.vtx[0]);
    }
}

//
// Create a new block with just given transactions, coinbase paying to
// scriptPubKey, and try to add it to the current chain.
//
CBlock TestChain100Setup::CreateAndProcessBlock(
    const std::vector<CMutableTransaction> &txns, const CScript &scriptPubKey) {
    const CChainParams &chainparams = Params();
    const Config &config = GetConfig();
    std::unique_ptr<CBlockTemplate> pblocktemplate =
        BlockAssembler(config, chainparams).CreateNewBlock(scriptPubKey);
    CBlock &block = pblocktemplate->block;

    // Replace mempool-selected txns with just coinbase plus passed-in txns:
    block.vtx.resize(1);
    for (const CMutableTransaction &tx : txns) {
        block.vtx.push_back(MakeTransactionRef(tx));
    }
    // IncrementExtraNonce creates a valid coinbase and merkleRoot
    unsigned int extraNonce = 0;
    IncrementExtraNonce(config, &block, chainActive.Tip(), extraNonce);

    while (!CheckProofOfWork(block.GetHash(), block.nBits,
                             chainparams.GetConsensus())) {
        ++block.nNonce;
    }

    std::shared_ptr<const CBlock> shared_pblock =
        std::make_shared<const CBlock>(block);
    ProcessNewBlock(GetConfig(), shared_pblock, true, nullptr);

    CBlock result = block;
    return result;
}

TestChain100Setup::~TestChain100Setup() {}

CTxMemPoolEntry TestMemPoolEntryHelper::FromTx(const CMutableTransaction &tx,
                                               CTxMemPool *pool) {
    CTransaction txn(tx);
    return FromTx(txn, pool);
}

CTxMemPoolEntry TestMemPoolEntryHelper::FromTx(const CTransaction &txn,
                                               CTxMemPool *pool) {
    // Hack to assume either it's completely dependent on other mempool txs or
    // not at all.
    CAmount inChainValue =
        pool && pool->HasNoInputsOf(txn) ? txn.GetValueOut() : 0;

    return CTxMemPoolEntry(MakeTransactionRef(txn), nFee, nTime, dPriority,
                           nHeight, inChainValue, spendsCoinbase, sigOpCost,
                           lp);
}

void Shutdown(void *parg) {
    exit(0);
}

void StartShutdown() {
    exit(0);
}

bool ShutdownRequested() {
    return false;
}
