// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#include <map>

#include <boost/version.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <leveldb/env.h>
#include <leveldb/cache.h>
#include <leveldb/filter_policy.h>
#include <memenv/memenv.h>

#include <thread>

#include "checkpoints.h"
#include "kernel.h"
#include "main.h"
#include "scrypt_mine.h"
#include "txdb-leveldb.h"
#include "util.h"
#include "init.h"
#include "wallet.h"

using namespace std;
using namespace boost;

extern CWallet* pwalletMain;
extern std::map<int, unsigned int> mapStakeModifierCheckpoints;
extern void scrypt_hash_mine(const void* input, size_t inputlen, uint32_t *res, void *scratchpad);
leveldb::DB *txdb; // global pointer for LevelDB object instance

static leveldb::Options GetOptions() {
    leveldb::Options options;
    int nCacheSizeMB = GetArg("-dbcache", 25);
    options.block_cache = leveldb::NewLRUCache(nCacheSizeMB * 1048576);
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    return options;
}

void init_blockindex(leveldb::Options& options, bool fRemoveOld = false)
{
    // First time init.
    filesystem::path directory = GetDataDir() / "txleveldb";

    if (fRemoveOld) {
        filesystem::remove_all(directory); // remove directory
        unsigned int nFile = 1;

        while (true)
        {
            filesystem::path strBlockFile = GetDataDir() / strprintf("blk%04u.dat", nFile);

            // Break if no such file
            if( !filesystem::exists( strBlockFile ) )
                break;

            filesystem::remove(strBlockFile);

            nFile++;
        }
    }

    filesystem::create_directory(directory);
    printf("Opening LevelDB in %s\n", directory.string().c_str());
    leveldb::Status status = leveldb::DB::Open(options, directory.string(), &txdb);
    if (!status.ok()) {
        throw runtime_error(strprintf("init_blockindex(): error opening database environment %s", status.ToString().c_str()));
    }
}

// CDB subclasses are created and destroyed VERY OFTEN. That's why
// we shouldn't treat this as a free operations.
CTxDB::CTxDB(const char* pszMode)
{
    assert(pszMode);
    activeBatch = NULL;
    fReadOnly = (!strchr(pszMode, '+') && !strchr(pszMode, 'w'));

    if (txdb) {
        pdb = txdb;
        return;
    }

    bool fCreate = strchr(pszMode, 'c');

    options = GetOptions();
    options.create_if_missing = fCreate;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);

    init_blockindex(options); // Init directory
    pdb = txdb;

    if (Exists(string("version")))
    {
        ReadVersion(nVersion);
        printf("Transaction index version is %d\n", nVersion);

        if (nVersion < DATABASE_VERSION)
        {
            printf("Required index version is %d, removing old database\n", DATABASE_VERSION);

            // Leveldb instance destruction
            delete txdb;
            txdb = pdb = NULL;
            delete activeBatch;
            activeBatch = NULL;

            init_blockindex(options, true); // Remove directory and create new database
            pdb = txdb;

            bool fTmp = fReadOnly;
            fReadOnly = false;
            WriteVersion(DATABASE_VERSION); // Save transaction index version
            fReadOnly = fTmp;
        }
    }
    else if (fCreate)
    {
        bool fTmp = fReadOnly;
        fReadOnly = false;
        WriteVersion(DATABASE_VERSION);
        fReadOnly = fTmp;
    }

    printf("Opened LevelDB successfully\n");
}

void CTxDB::Close()
{
    delete txdb;
    txdb = pdb = NULL;
    delete options.filter_policy;
    options.filter_policy = NULL;
    delete options.block_cache;
    options.block_cache = NULL;
    delete activeBatch;
    activeBatch = NULL;
}

bool CTxDB::TxnBegin()
{
    assert(!activeBatch);
    activeBatch = new leveldb::WriteBatch();
    return true;
}

bool CTxDB::TxnCommit()
{
    assert(activeBatch);
    leveldb::Status status = pdb->Write(leveldb::WriteOptions(), activeBatch);
    delete activeBatch;
    activeBatch = NULL;
    if (!status.ok())
    {
        printf("LevelDB batch commit failure: %s\n", status.ToString().c_str());
        return false;
    }
    return true;
}

class CBatchScanner : public leveldb::WriteBatch::Handler {
public:
    std::string needle;
    bool *deleted;
    std::string *foundValue;
    bool foundEntry;

    CBatchScanner() : foundEntry(false) {}

    virtual void Put(const leveldb::Slice& key, const leveldb::Slice& value) {
        if (key.ToString() == needle) {
            foundEntry = true;
            *deleted = false;
            *foundValue = value.ToString();
        }
    }

    virtual void Delete(const leveldb::Slice& key) {
        if (key.ToString() == needle) {
            foundEntry = true;
            *deleted = true;
        }
    }
};

// When performing a read, if we have an active batch we need to check it first
// before reading from the database, as the rest of the code assumes that once
// a database transaction begins reads are consistent with it. It would be good
// to change that assumption in future and avoid the performance hit, though in
// practice it does not appear to be large.
bool CTxDB::ScanBatch(const CDataStream &key, string *value, bool *deleted) const {
    assert(activeBatch);
    *deleted = false;
    CBatchScanner scanner;
    scanner.needle = key.str();
    scanner.deleted = deleted;
    scanner.foundValue = value;
    leveldb::Status status = activeBatch->Iterate(&scanner);
    if (!status.ok())
    {
        throw runtime_error(status.ToString());
    }
    return scanner.foundEntry;
}

bool CTxDB::ReadTxIndex(uint256 hash, CTxIndex& txindex)
{
    assert(!fClient);
    txindex.SetNull();
    return Read(make_pair(string("tx"), hash), txindex);
}

bool CTxDB::UpdateTxIndex(uint256 hash, const CTxIndex& txindex)
{
    assert(!fClient);
    return Write(make_pair(string("tx"), hash), txindex);
}

bool CTxDB::AddTxIndex(const CTransaction& tx, const CDiskTxPos& pos, int nHeight)
{
    assert(!fClient);

    // Add to tx index
    uint256 hash = tx.GetHash();
    CTxIndex txindex(pos, tx.vout.size());
    return Write(make_pair(string("tx"), hash), txindex);
}

bool CTxDB::EraseTxIndex(const CTransaction& tx)
{
    assert(!fClient);
    uint256 hash = tx.GetHash();

    return Erase(make_pair(string("tx"), hash));
}

bool CTxDB::ContainsTx(uint256 hash)
{
    assert(!fClient);
    return Exists(make_pair(string("tx"), hash));
}

bool CTxDB::ReadDiskTx(uint256 hash, CTransaction& tx, CTxIndex& txindex)
{
    assert(!fClient);
    tx.SetNull();
    if (!ReadTxIndex(hash, txindex))
        return false;
    return (tx.ReadFromDisk(txindex.pos));
}

bool CTxDB::ReadDiskTx(uint256 hash, CTransaction& tx)
{
    CTxIndex txindex;
    return ReadDiskTx(hash, tx, txindex);
}

bool CTxDB::ReadDiskTx(COutPoint outpoint, CTransaction& tx, CTxIndex& txindex)
{
    return ReadDiskTx(outpoint.hash, tx, txindex);
}

bool CTxDB::ReadDiskTx(COutPoint outpoint, CTransaction& tx)
{
    CTxIndex txindex;
    return ReadDiskTx(outpoint.hash, tx, txindex);
}

bool CTxDB::WriteBlockIndex(const CDiskBlockIndex& blockindex)
{
    return Write(make_pair(string("blockindex"), blockindex.GetBlockHashScrypt()), blockindex);
}

bool CTxDB::ReadHashBestChain(uint256& hashBestChain)
{
    return Read(string("hashBestChain"), hashBestChain);
}

bool CTxDB::WriteHashBestChain(uint256 hashBestChain)
{
    return Write(string("hashBestChain"), hashBestChain);
}

bool CTxDB::ReadBestInvalidTrust(CBigNum& bnBestInvalidTrust)
{
    return Read(string("bnBestInvalidTrust"), bnBestInvalidTrust);
}

bool CTxDB::WriteBestInvalidTrust(CBigNum bnBestInvalidTrust)
{
    return Write(string("bnBestInvalidTrust"), bnBestInvalidTrust);
}

bool CTxDB::ReadSyncCheckpoint(uint256& hashCheckpoint)
{
    return Read(string("hashSyncCheckpoint"), hashCheckpoint);
}

bool CTxDB::WriteSyncCheckpoint(uint256 hashCheckpoint)
{
    return Write(string("hashSyncCheckpoint"), hashCheckpoint);
}

bool CTxDB::ReadCheckpointPubKey(string& strPubKey)
{
    return Read(string("strCheckpointPubKey"), strPubKey);
}

bool CTxDB::WriteCheckpointPubKey(const string& strPubKey)
{
    return Write(string("strCheckpointPubKey"), strPubKey);
}

static CBlockIndex *InsertBlockIndex(uint256 hash)
{
    if (hash == 0)
        return NULL;

    // Return existing
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end())
        return (*mi).second;

    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    if (!pindexNew)
        throw runtime_error("LoadBlockIndex() : new CBlockIndex failed");
    mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

void ThreadForFinishBlockIndex()
{
    CTxDB txdb("cr+");
    printf("starting finishblockindex() \n");
    txdb.FinishBlockIndex();
    printf("\n\n\n ~~~ load thread exited~~~ \n\n\n");
}

void ThreadForReadingTx(vector<CBlockIndex*> checklist)
{
    pwalletMain->UpdatedTransactionBasedOnList(checklist);
}

void CTxDB::FinishBlockIndex() // this is old and causes an error with allowing double spends
{
    leveldb::Iterator *iterator = pdb->NewIterator(leveldb::ReadOptions());

    CDataStream ssStartKey(SER_DISK, CLIENT_VERSION);
    ssStartKey << make_pair(string("blockindex"), uint256(0));
    iterator->Seek(ssStartKey.str());
    std::vector<CBlockIndex*> checklist;
    int round = 0;
    while(iterator->Valid())
   {
        // Unpack keys and values.
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.write(iterator->key().data(), iterator->key().size());
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        ssValue.write(iterator->value().data(), iterator->value().size());
        string strType;
        ssKey >> strType;
        // Did we reach the end of the data to read?
        if (fRequestShutdown || strType != "blockindex")
            break;
        CDiskBlockIndex diskindex;
        ssValue >> diskindex;

        uint256 blockHash = diskindex.GetBlockHashScrypt();
        if(mapBlockIndex.find(blockHash) == mapBlockIndex.end())
        {
            // Construct block index object
            CBlockIndex* pindexNew      = InsertBlockIndex(blockHash);
            pindexNew->pprev            = InsertBlockIndex(diskindex.hashPrev);
            pindexNew->pnext            = InsertBlockIndex(diskindex.hashNext);
            pindexNew->nFile            = diskindex.nFile;
            pindexNew->nBlockPos        = diskindex.nBlockPos;
            pindexNew->nHeight          = diskindex.nHeight;
            pindexNew->nMint            = diskindex.nMint;
            pindexNew->nMoneySupply     = diskindex.nMoneySupply;
            pindexNew->nFlags           = diskindex.nFlags;
            pindexNew->nStakeModifier   = diskindex.nStakeModifier;
            pindexNew->prevoutStake     = diskindex.prevoutStake;
            pindexNew->nStakeTime       = diskindex.nStakeTime;
            pindexNew->hashProofOfStake = diskindex.hashProofOfStake;
            pindexNew->nVersion         = diskindex.nVersion;
            pindexNew->hashMerkleRoot   = diskindex.hashMerkleRoot;
            pindexNew->nTime            = diskindex.nTime;
            pindexNew->nBits            = diskindex.nBits;
            pindexNew->nNonce           = diskindex.nNonce;

            checklist.push_back(pindexNew); // add to a list of blocks to be checked
            // Watch for genesis block
            if (pindexGenesisBlock == NULL && blockHash == (!fTestNet ? hashGenesisBlock : hashGenesisBlockTestNet))
                pindexGenesisBlock = pindexNew;
            // NovaCoin: build setStakeSeen
            if (pindexNew->IsProofOfStake())
                setStakeSeen.insert(make_pair(pindexNew->prevoutStake, pindexNew->nStakeTime));
            round = round + 1;
            if(round >= 25000)
            {
               /// this works on win but not on mac, not sure why, not used atm anyway so it is commented out
               ///std::thread loadTx (ThreadForReadingTx,checklist);     // spawn new thread
               ///loadTx.detach();
               ///
                round = 0;
                checklist.clear();
            }
        }
        if(fShutdown)
        {
            return;
        }
        iterator->Next();
   }
   delete iterator;
   printf("DEBUG: FinishBlockIndex(): mapBlockIndex.size = %i \n", mapBlockIndex.size());

   /// this works on win but not on mac, not sure why, not used atm anyway so it is commented out
   ///std::thread loadTx (ThreadForReadingTx,checklist);     // spawn new thread
   ///loadTx.detach();
   ///

   round = 0;
   checklist.clear();
   printf("************************************ finished******************************************\n" );
   return;

}

bool CTxDB::LoadBlockIndex()
{
    // The block index is an in-memory structure that maps hashes to on-disk
    // locations where the contents of the block can be found. Here, we scan it
    // out of the DB and into mapBlockIndex.
    leveldb::Iterator *SmallIterator = pdb->NewIterator(leveldb::ReadOptions());
    // Seek to start key.
    CDataStream smallStartKey(SER_DISK, CLIENT_VERSION);
    smallStartKey << make_pair(string("blockindex"), uint256(0));
    SmallIterator->Seek(smallStartKey.str());

    /// load only the blocks until the last checkpoint
    /// as the chain gets longer it will become less and less likely
    /// that we will need the whole chain to validate something
    /// so only load the last 25k blocks. if there is a reorg
    /// larger than 25k then something is obviously wrong anyway
    /// also we load the rest in a seperate thread later


    //get the total number of blocks we have on the disk
    unsigned int TotalNumBlocks = 0;
    while (SmallIterator->Valid())
    {
        TotalNumBlocks = TotalNumBlocks + 1;
        //this is a check to see if we hit the end of the data, dont load values because it doesnt matter
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.write(SmallIterator->key().data(), SmallIterator->key().size());
        string strType;
        ssKey >> strType;
        if (fRequestShutdown || strType != "blockindex")
            break;
        SmallIterator->Next();
    }
    delete SmallIterator;

    leveldb::Iterator *iterator = pdb->NewIterator(leveldb::ReadOptions());
    printf("DEBUG: Total number of blocks found in blockchain = %u \n", TotalNumBlocks);


    ///this code reduces rescan time

    unsigned int bestCheckpoint = 0;
    typedef std::map<int, uint256>::iterator it_type;
    for(it_type CheckpointIterator = mapCheckpoints.begin(); CheckpointIterator != mapCheckpoints.end(); CheckpointIterator++)
    {
        unsigned int currentBest = CheckpointIterator->first;
        if (TotalNumBlocks - 250 < 0)
        {
            TotalNumBlocks = 0;
        }
        if(currentBest < (TotalNumBlocks))
        {
            bestCheckpoint = currentBest;
        }
    }
/*
        uint256 startingblock;
        if (bestCheckpoint != 0)
        {
            startingblock = mapCheckpoints.find(bestCheckpoint)->second;
        }
        else
        {
            startingblock = uint256(0);
        }

        */
        bool AdditionalThreading = false;
        //printf("DEBUG: loading from checkpoint block# : %i, hash: %s \n",loaded, startingblock.ToString().c_str());
        CDataStream ssStartKey(SER_DISK, CLIENT_VERSION);
        ssStartKey << make_pair(string("blockindex"), uint256(0));
        iterator->Seek(ssStartKey.str());
        std::vector<CDiskBlockIndex> vDiskBlockIndex;
            AdditionalThreading = false;
            while (iterator->Valid())
                {
                    // Unpack keys and values.
                    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
                    ssKey.write(iterator->key().data(), iterator->key().size());
                    CDataStream ssValue(SER_DISK, CLIENT_VERSION);
                    ssValue.write(iterator->value().data(), iterator->value().size());
                    string strType;
                    ssKey >> strType;
                    // Did we reach the end of the data to read?
                    if (fRequestShutdown || strType != "blockindex")
                        break;
                    CDiskBlockIndex diskindex;
                    ssValue >> diskindex;
                    vDiskBlockIndex.push_back(diskindex);

                    iterator->Next();
                }

            for(unsigned int i = 0; i < vDiskBlockIndex.size(); i++)
            {
                CDiskBlockIndex diskindex = vDiskBlockIndex[i];

                uint256 blockHash = diskindex.GetBlockHash();

                // Construct block index object
                CBlockIndex* pindexNew    = InsertBlockIndex(blockHash);
                pindexNew->pprev          = InsertBlockIndex(diskindex.hashPrev);
                pindexNew->pnext          = InsertBlockIndex(diskindex.hashNext);
                pindexNew->nFile          = diskindex.nFile;
                pindexNew->nBlockPos      = diskindex.nBlockPos;
                pindexNew->nHeight        = diskindex.nHeight;
                pindexNew->nMint          = diskindex.nMint;
                pindexNew->nMoneySupply   = diskindex.nMoneySupply;
                pindexNew->nFlags         = diskindex.nFlags;
                pindexNew->nStakeModifier = diskindex.nStakeModifier;
                pindexNew->prevoutStake   = diskindex.prevoutStake;
                pindexNew->nStakeTime     = diskindex.nStakeTime;
                pindexNew->hashProofOfStake = diskindex.hashProofOfStake;
                pindexNew->nVersion       = diskindex.nVersion;
                pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
                pindexNew->nTime          = diskindex.nTime;
                pindexNew->nBits          = diskindex.nBits;
                pindexNew->nNonce         = diskindex.nNonce;

                // Watch for genesis block
                if (pindexGenesisBlock == NULL && blockHash == (!fTestNet ? hashGenesisBlock : hashGenesisBlockTestNet))
                    pindexGenesisBlock = pindexNew;

                // NovaCoin: build setStakeSeen
                if (pindexNew->IsProofOfStake())
                    setStakeSeen.insert(make_pair(pindexNew->prevoutStake, pindexNew->nStakeTime));
            }
        //}

    delete iterator;
    vDiskBlockIndex.clear();

    if (fRequestShutdown)
        return true;


    // Calculate nChainTrust
    vector<pair<int, CBlockIndex*> > vSortedByHeight;
    printf("DEBUG: mapBlockIndex.size = %i \n", mapBlockIndex.size());
    vSortedByHeight.reserve(mapBlockIndex.size());

    BOOST_FOREACH(const PAIRTYPE(uint256, CBlockIndex*)& item, mapBlockIndex)
    {
        CBlockIndex* pindex = item.second;
        vSortedByHeight.push_back(make_pair(pindex->nHeight, pindex));
    }
    sort(vSortedByHeight.begin(), vSortedByHeight.end());

    BOOST_FOREACH(const PAIRTYPE(int, CBlockIndex*)& item, vSortedByHeight)
        {
                CBlockIndex* pindex = item.second;
                pindex->nChainTrust = (pindex->pprev ? pindex->pprev->nChainTrust : 0) + pindex->GetBlockTrust();
        }
    // Load hashBestChain pointer to end of best chain
    if (!ReadHashBestChain(hashBestChain))
    {
        if (pindexGenesisBlock == NULL)
            return true;
        return error("CTxDB::LoadBlockIndex() : hashBestChain not loaded");
    }
    if (!mapBlockIndex.count(hashBestChain))
        return error("CTxDB::LoadBlockIndex() : hashBestChain not found in the block index");
    pindexBest = mapBlockIndex[hashBestChain];
        nBestHeight = pindexBest->nHeight;
        nBestChainTrust = pindexBest->nChainTrust;

            // ECCoin: write checkpoint we loaded from
            if( bestCheckpoint != 0 )
            {
                uint256 CheckpointBlock = mapCheckpoints.find(bestCheckpoint)->second;
                if (!Checkpoints::WriteSyncCheckpoint(CheckpointBlock))
                    return error("LoadBlockIndex() : failed to init sync checkpoint");
            }
            else
            {
                if (!Checkpoints::WriteSyncCheckpoint((!fTestNet ? hashGenesisBlock : hashGenesisBlockTestNet)))
                    return error("LoadBlockIndex() : failed to init sync checkpoint");
            }

            // NovaCoin: load hashSyncCheckpoint
            if (!ReadSyncCheckpoint(Checkpoints::hashSyncCheckpoint))
                return error("CTxDB::LoadBlockIndex() : hashSyncCheckpoint not loaded");
            printf("LoadBlockIndex(): synchronized checkpoint %s\n", Checkpoints::hashSyncCheckpoint.ToString().c_str());

            // Load bnBestInvalidTrust, OK if it doesn't exist
            CBigNum bnBestInvalidTrust;
            ReadBestInvalidTrust(bnBestInvalidTrust);
            nBestInvalidTrust = bnBestInvalidTrust.getuint256();

            // Verify blocks in the best chain
            int nCheckLevel = GetArg("-checklevel", 1);
            int nCheckDepth = GetArg( "-checkblocks", 2500);
            if (nCheckDepth == 0)
                nCheckDepth = 1000000000; // suffices until the year 19000
            if (nCheckDepth > nBestHeight)
                nCheckDepth = nBestHeight;
            printf("Verifying last %i blocks at level %i\n", nCheckDepth, nCheckLevel);
            CBlockIndex* pindexFork = NULL;

            map<pair<unsigned int, unsigned int>, CBlockIndex*> mapBlockPos;
        for (CBlockIndex* pindex = pindexBest; pindex && pindex->pprev; pindex = pindex->pprev)
        {
            if (fRequestShutdown || pindex->nHeight < nBestHeight-nCheckDepth)
                break;
            CBlock block;
            if (!block.ReadFromDisk(pindex))
                return error("LoadBlockIndex() : block.ReadFromDisk failed");
            // check level 1: verify block validity
            // check level 7: verify block signature too
            if (nCheckLevel>0 && !block.CheckBlock(true, true, (nCheckLevel>6)))
            {
                printf("LoadBlockIndex() : *** found bad block at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
                pindexFork = pindex->pprev;
            }
            // check level 2: verify transaction index validity
            if (nCheckLevel>1)
            {
                pair<unsigned int, unsigned int> pos = make_pair(pindex->nFile, pindex->nBlockPos);
                mapBlockPos[pos] = pindex;
                BOOST_FOREACH(const CTransaction &tx, block.vtx)
                {
                    uint256 hashTx = tx.GetHash();
                    CTxIndex txindex;
                    if (ReadTxIndex(hashTx, txindex))
                    {
                        // check level 3: checker transaction hashes
                        if (nCheckLevel>2 || pindex->nFile != txindex.pos.nFile || pindex->nBlockPos != txindex.pos.nBlockPos)
                        {
                            // either an error or a duplicate transaction
                            CTransaction txFound;
                            if (!txFound.ReadFromDisk(txindex.pos))
                            {
                                printf("LoadBlockIndex() : *** cannot read mislocated transaction %s\n", hashTx.ToString().c_str());
                                pindexFork = pindex->pprev;
                            }
                            else
                                if (txFound.GetHash() != hashTx) // not a duplicate tx
                                {
                                    printf("LoadBlockIndex(): *** invalid tx position for %s\n", hashTx.ToString().c_str());
                                    pindexFork = pindex->pprev;
                                }
                        }
                        // check level 4: check whether spent txouts were spent within the main chain
                        unsigned int nOutput = 0;
                        if (nCheckLevel>3)
                        {
                            BOOST_FOREACH(const CDiskTxPos &txpos, txindex.vSpent)
                            {
                                if (!txpos.IsNull())
                                {
                                    pair<unsigned int, unsigned int> posFind = make_pair(txpos.nFile, txpos.nBlockPos);
                                    if (!mapBlockPos.count(posFind))
                                    {
                                        printf("LoadBlockIndex(): *** found bad spend at %d, hashBlock=%s, hashTx=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString().c_str(), hashTx.ToString().c_str());
                                        pindexFork = pindex->pprev;
                                    }
                                    // check level 6: check whether spent txouts were spent by a valid transaction that consume them
                                    if (nCheckLevel>5)
                                    {
                                        CTransaction txSpend;
                                        if (!txSpend.ReadFromDisk(txpos))
                                        {
                                            printf("LoadBlockIndex(): *** cannot read spending transaction of %s:%i from disk\n", hashTx.ToString().c_str(), nOutput);
                                            pindexFork = pindex->pprev;
                                        }
                                        else if (!txSpend.CheckTransaction())
                                        {
                                            printf("LoadBlockIndex(): *** spending transaction of %s:%i is invalid\n", hashTx.ToString().c_str(), nOutput);
                                            pindexFork = pindex->pprev;
                                        }
                                        else
                                        {
                                            bool fFound = false;
                                            BOOST_FOREACH(const CTxIn &txin, txSpend.vin)
                                                if (txin.prevout.hash == hashTx && txin.prevout.n == nOutput)
                                                    fFound = true;
                                            if (!fFound)
                                            {
                                                printf("LoadBlockIndex(): *** spending transaction of %s:%i does not spend it\n", hashTx.ToString().c_str(), nOutput);
                                                pindexFork = pindex->pprev;
                                            }
                                        }
                                    }
                                }
                                nOutput++;
                            }
                        }
                    }
                    // check level 5: check whether all prevouts are marked spent
                    if (nCheckLevel>4)
                    {
                         BOOST_FOREACH(const CTxIn &txin, tx.vin)
                         {
                              CTxIndex txindex;
                              if (ReadTxIndex(txin.prevout.hash, txindex))
                                  if (txindex.vSpent.size()-1 < txin.prevout.n || txindex.vSpent[txin.prevout.n].IsNull())
                                  {
                                      printf("LoadBlockIndex(): *** found unspent prevout %s:%i in %s\n", txin.prevout.hash.ToString().c_str(), txin.prevout.n, hashTx.ToString().c_str());
                                      pindexFork = pindex->pprev;
                                  }
                         }
                    }
                }
            }
         }
        if (pindexFork && !fRequestShutdown)
        {
            // Reorg back to the fork
            printf("LoadBlockIndex() : *** moving best chain pointer back to block %d\n", pindexFork->nHeight);
            CBlock block;
            if (!block.ReadFromDisk(pindexFork))
                return error("LoadBlockIndex() : block.ReadFromDisk failed");
            CTxDB txdb;
            block.SetBestChain(txdb, pindexFork);
        }

//        if(AdditionalThreading == true)
//        {
//            std::thread FinishBlockIndex (ThreadForFinishBlockIndex);     // spawn new thread to laod rest of chain
//            FinishBlockIndex.detach();
//        }

        printf("best block loaded: %s\n", pindexBest->ToString().c_str());

    return true;
}