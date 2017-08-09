// Copyright (c) 2011-2013 The Bitcoin Core developers
// Copyright (c) 2017 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test_ion.h"

#include "chainparams.h"
#include "consensus/consensus.h"
#include "consensus/validation.h"
#include "crypto/sha256.h"
#include "fs.h"
#include "key.h"
#include "validation.h"
#include "miner.h"
#include "net_processing.h"
#include "pubkey.h"
#include "random.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "util.h"
#ifdef ENABLE_WALLET
#include "wallet/db.h"
#include "wallet/wallet.h"
#endif

#include "evo/specialtx.h"
#include "evo/deterministicmns.h"
#include "evo/cbtx.h"
#include "llmq/quorums_init.h"

#include <memory>

void CConnmanTest::AddNode(CNode& node)
{
    LOCK(g_connman->cs_vNodes);
    g_connman->vNodes.push_back(&node);
}

void CConnmanTest::ClearNodes()
{
    LOCK(g_connman->cs_vNodes);
    g_connman->vNodes.clear();
}

uint256 insecure_rand_seed = GetRandHash();
FastRandomContext insecure_rand_ctx(insecure_rand_seed);

extern bool fPrintToConsole;
extern void noui_connect();

struct TestingSetup {
    boost::filesystem::path pathTemp;
    boost::thread_group threadGroup;
    ECCVerifyHandle globalVerifyHandle;

    TestingSetup() {
        ECC_Start();
        BLSInit();
        SetupEnvironment();
        SetupNetworking();
        InitSignatureCache();
        InitScriptExecutionCache();
        fPrintToDebugLog = false; // don't want to write to debug.log file
        fCheckBlockIndex = true;
        SelectParams(chainName);
        evoDb = new CEvoDB(1 << 20, true, true);
        deterministicMNManager = new CDeterministicMNManager(*evoDb);
        noui_connect();
#ifdef ENABLE_WALLET
        bitdb.MakeMock();
#endif
        pathTemp = GetTempPath() / strprintf("test_ion_%lu_%i", (unsigned long)GetTime(), (int)(GetRand(100000)));
        boost::filesystem::create_directories(pathTemp);
        mapArgs["-datadir"] = pathTemp.string();
        pblocktree.reset(new CBlockTreeDB(1 << 20, true));
        pcoinsdbview.reset(new CCoinsViewDB(1 << 23, true));
        pcoinsTip.reset(new CCoinsViewCache(pcoinsdbview.get()));
        InitBlockIndex();
#ifdef ENABLE_WALLET
        bool fFirstRun;
        pwalletMain = new CWallet("wallet.dat");
        pwalletMain->LoadWallet(fFirstRun);
        RegisterValidationInterface(pwalletMain);
#endif
        nScriptCheckThreads = 3;
        for (int i=0; i < nScriptCheckThreads-1; i++)
            threadGroup.create_thread(&ThreadScriptCheck);
        peerLogic.reset(new PeerLogicValidation(connman, scheduler));
}

TestingSetup::~TestingSetup()
{
        llmq::InterruptLLMQSystem();
        threadGroup.interrupt_all();
        threadGroup.join_all();
        UnregisterNodeSignals(GetNodeSignals());
#ifdef ENABLE_WALLET
        delete pwalletMain;
        pwalletMain = NULL;
#endif
        pcoinsTip.reset();
        pcoinsdbview.reset();
        pblocktree.reset();
#ifdef ENABLE_WALLET
        bitdb.Flush(true);
#endif
        boost::filesystem::remove_all(pathTemp);
        ECC_Stop();
    }
}

//
// Create a new block with just given transactions, coinbase paying to
// scriptPubKey, and try to add it to the current chain.
//
CBlock
TestChainSetup::CreateAndProcessBlock(const std::vector<CMutableTransaction>& txns, const CScript& scriptPubKey)
{
    const CChainParams& chainparams = Params();
    auto block = CreateBlock(txns, scriptPubKey);

    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    ProcessNewBlock(chainparams, shared_pblock, true, nullptr);

    CBlock result = block;
    return result;
}

CBlock TestChainSetup::CreateAndProcessBlock(const std::vector<CMutableTransaction>& txns, const CKey& scriptKey)
{
    CScript scriptPubKey = CScript() <<  ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    return CreateAndProcessBlock(txns, scriptPubKey);
}

CBlock TestChainSetup::CreateBlock(const std::vector<CMutableTransaction>& txns, const CScript& scriptPubKey)
{
    const CChainParams& chainparams = Params();
    std::unique_ptr<CBlockTemplate> pblocktemplate = BlockAssembler(chainparams).CreateNewBlock(scriptPubKey);
    CBlock& block = pblocktemplate->block;

    std::vector<CTransactionRef> llmqCommitments;
    for (const auto& tx : block.vtx) {
        if (tx->nVersion == 3 && tx->nType == TRANSACTION_QUORUM_COMMITMENT) {
            llmqCommitments.emplace_back(tx);
        }
    }

    // Replace mempool-selected txns with just coinbase plus passed-in txns:
    block.vtx.resize(1);
    // Re-add quorum commitments
    block.vtx.insert(block.vtx.end(), llmqCommitments.begin(), llmqCommitments.end());
    for (const CMutableTransaction& tx : txns)
        block.vtx.push_back(MakeTransactionRef(tx));

    // Manually update CbTx as we modified the block here
    if (block.vtx[0]->nType == TRANSACTION_COINBASE) {
        LOCK(cs_main);
        CCbTx cbTx;
        if (!GetTxPayload(*block.vtx[0], cbTx)) {
            BOOST_ASSERT(false);
        }
        CValidationState state;
        if (!CalcCbTxMerkleRootMNList(block, chainActive.Tip(), cbTx.merkleRootMNList, state)) {
            BOOST_ASSERT(false);
        }
        if (!CalcCbTxMerkleRootQuorums(block, chainActive.Tip(), cbTx.merkleRootQuorums, state)) {
            BOOST_ASSERT(false);
        }
        CMutableTransaction tmpTx = *block.vtx[0];
        SetTxPayload(tmpTx, cbTx);
        block.vtx[0] = MakeTransactionRef(tmpTx);
    }

    // IncrementExtraNonce creates a valid coinbase and merkleRoot
    unsigned int extraNonce = 0;
    IncrementExtraNonce(&block, chainActive.Tip(), extraNonce);

    while (!CheckProofOfWork(block.GetHash(), block.nBits, chainparams.GetConsensus())) ++block.nNonce;

    CBlock result = block;
    return result;
}

CBlock TestChainSetup::CreateBlock(const std::vector<CMutableTransaction>& txns, const CKey& scriptKey)
{
    CScript scriptPubKey = CScript() <<  ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    return CreateBlock(txns, scriptPubKey);
}

TestChainSetup::~TestChainSetup()
{
}


CTxMemPoolEntry TestMemPoolEntryHelper::FromTx(const CMutableTransaction &tx) {
    CTransaction txn(tx);
    return FromTx(txn);
}

CTxMemPoolEntry TestMemPoolEntryHelper::FromTx(const CTransaction &txn) {
    return CTxMemPoolEntry(MakeTransactionRef(txn), nFee, nTime, nHeight,
                           spendsGenerated, sigOpCount, lp);
}
