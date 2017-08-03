// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014,2015 The BlackCoin developers
// Copyright (c) 2015,2016 James C. Stroud
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp>

#include "kernel.h"
#include "txdb.h"

using namespace std;

typedef std::map<int, unsigned int> MapModifierCheckpoints;

// Hard checkpoints of stake modifier checksums to ensure they are deterministic
static std::map<int, unsigned int> mapStakeModifierCheckpoints =
    boost::assign::map_list_of
        (         0, 0x7f061366u )
        (     37500, 0x7ac1cf6bu )
        (     87000, 0x3e355b36u )
        (    107000, 0x88ff8fc9u )
    ;

// Hard checkpoints of stake modifier checksums to ensure they are deterministic (testNet)
static std::map<int, unsigned int> mapStakeModifierCheckpointsTestNet =
    boost::assign::map_list_of
        (         0, 0xc178d797u )
    ;

// Get time weight
int64_t GetWeight(int64_t nIntervalBeginning, int64_t nIntervalEnd, int nColor, int64_t nTimeBlockPrev)
{
    // ADVISORY: static is an optimization and may not be suitable
    static unsigned int nStakeMinAge = GetStakeMinAge();
    static unsigned int nStakeMaxAge = GetStakeMaxAge();
    if (!CanStake(nColor))
    {
           printf("Currency not valid: %d\n", nColor);
           return 0;
    }
    // Kernel hash weight starts from 0 at the min age
    // this change increases active coins participating the hash and helps
    // to secure the network when proof-of-stake difficulty is low
    int64_t d = nIntervalEnd - nIntervalBeginning - nStakeMinAge;
    return min(d, (int64_t)nStakeMaxAge) * GetWeightMultiplier(nColor, nTimeBlockPrev);
}

// Get the last stake modifier and its generation time from a given block
static bool GetLastStakeModifier(const CBlockIndex* pindex, uint256& bnStakeModifier, int64_t& nModifierTime)
{
    if (!pindex)
        return error("GetLastStakeModifier: null pindex");
    while (pindex && pindex->pprev && !pindex->GeneratedStakeModifier())
        pindex = pindex->pprev;
    if (!pindex->GeneratedStakeModifier())
        return error("GetLastStakeModifier: no generation at genesis block");
    bnStakeModifier = pindex->bnStakeModifier;
    nModifierTime = pindex->GetBlockTime();
    return true;
}

// Get selection interval section (in seconds)
static int64_t GetStakeModifierSelectionIntervalSection(int nSection)
{
    // ADVISORY: static is optimization, may not be appropriate for forks
    static unsigned int nModifierInterval = GetModifierInterval();
    assert (nSection >= 0 && nSection < 64);
    return (nModifierInterval * 63 / (63 + ((63 - nSection) * (MODIFIER_INTERVAL_RATIO - 1))));
}

// Get stake modifier selection interval (in seconds)
static int64_t GetStakeModifierSelectionInterval()
{
    int64_t nSelectionInterval = 0;
    for (int nSection=0; nSection<64; nSection++)
        nSelectionInterval += GetStakeModifierSelectionIntervalSection(nSection);
    return nSelectionInterval;
}

// select a block from the candidate blocks in vSortedByTimestamp, excluding
// already selected blocks in vSelectedBlocks, and with timestamp up to
// nSelectionIntervalStop.
static bool SelectBlockFromCandidates(vector<pair<int64_t, uint256> >& vSortedByTimestamp, map<uint256, const CBlockIndex*>& mapSelectedBlocks,
    int64_t nSelectionIntervalStop, uint256 bnStakeModifierPrev, const CBlockIndex** pindexSelected)
{
    bool fSelected = false;
    uint256 hashBest = 0;
    *pindexSelected = (const CBlockIndex*) 0;
    BOOST_FOREACH(const PAIRTYPE(int64_t, uint256)& item, vSortedByTimestamp)
    {
        if (!mapBlockIndex.count(item.second))
            return error("SelectBlockFromCandidates: failed to find block index for candidate block %s", item.second.ToString().c_str());
        const CBlockIndex* pindex = mapBlockIndex[item.second];
        if (fSelected && pindex->GetBlockTime() > nSelectionIntervalStop)
            break;
        if (mapSelectedBlocks.count(pindex->GetBlockHash()) > 0)
            continue;
        // compute the selection hash by hashing its proof-hash and the
        // previous proof-of-stake modifier
        CDataStream ss(SER_GETHASH, 0);
        ss << pindex->hashProof << bnStakeModifierPrev;
        uint256 hashSelection = Hash(ss.begin(), ss.end());
        // the selection hash is divided by 2**32 so that proof-of-stake block
        // is always favored over proof-of-work block. this is to preserve
        // the energy efficiency property
        if (pindex->IsProofOfStake())
            hashSelection >>= 32;
        if (fSelected && hashSelection < hashBest)
        {
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
        else if (!fSelected)
        {
            fSelected = true;
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
    }
    if (fDebug && GetBoolArg("-printstakemodifier"))
        printf("SelectBlockFromCandidates: selection hash=%s\n", hashBest.ToString().c_str());
    return fSelected;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
// Stake modifier consists of bits each of which is contributed from a
// selected block of a given block group in the past.
// The selection of a block is based on a hash of the block's proof-hash and
// the previous stake modifier.
// Stake modifier is recomputed at a fixed time interval instead of every 
// block. This is to make it difficult for an attacker to gain control of
// additional bits in the stake modifier, even after generating a chain of
// blocks.
bool ComputeNextStakeModifier(const CBlockIndex* pindexPrev, uint256& bnStakeModifier, bool& fGeneratedStakeModifier)
{
    // ADVISORY: static is optimization, may not be appropriate for forks
    static unsigned int nModifierInterval = GetModifierInterval();

    bnStakeModifier = 0;
    fGeneratedStakeModifier = false;
    if (!pindexPrev)
    {
        fGeneratedStakeModifier = true;
        return true;  // genesis block's modifier is 0
    }
    // First find current stake modifier and its generation block time
    // if it's not old enough, return the same stake modifier
    int64_t nModifierTime = 0;
    if (!GetLastStakeModifier(pindexPrev, bnStakeModifier, nModifierTime))
        return error("ComputeNextStakeModifier: unable to get last modifier");
    if (fDebug)
    {
        printf("ComputeNextStakeModifier: prev modifier=%s time=%s\n", bnStakeModifier.ToString().c_str(), DateTimeStrFormat(nModifierTime).c_str());
    }
    if (nModifierTime / nModifierInterval >= pindexPrev->GetBlockTime() / nModifierInterval)
        return true;

    // Sort candidate blocks by timestamp
    vector<pair<int64_t, uint256> > vSortedByTimestamp;
    int nTargetSpacing = GetTargetSpacing(true);
    vSortedByTimestamp.reserve(64 * nModifierInterval / nTargetSpacing);
    int64_t nSelectionInterval = GetStakeModifierSelectionInterval();
    int64_t nSelectionIntervalStart = (pindexPrev->GetBlockTime() / nModifierInterval) * nModifierInterval - nSelectionInterval;
    const CBlockIndex* pindex = pindexPrev;
    while (pindex && pindex->GetBlockTime() >= nSelectionIntervalStart)
    {
        vSortedByTimestamp.push_back(make_pair(pindex->GetBlockTime(), pindex->GetBlockHash()));
        pindex = pindex->pprev;
    }
    int nHeightFirstCandidate = pindex ? (pindex->nHeight + 1) : 0;
    reverse(vSortedByTimestamp.begin(), vSortedByTimestamp.end());
    sort(vSortedByTimestamp.begin(), vSortedByTimestamp.end());

    // Select 64 blocks from candidate blocks to generate stake modifier
    uint256 bnStakeModifierNew = 0;
    int64_t nSelectionIntervalStop = nSelectionIntervalStart;
    map<uint256, const CBlockIndex*> mapSelectedBlocks;
    for (int nRound=0; nRound<min(64, (int)vSortedByTimestamp.size()); nRound++)
    {
        // add an interval section to the current selection round
        nSelectionIntervalStop += GetStakeModifierSelectionIntervalSection(nRound);
        // select a block from the candidates of current round
        if (!SelectBlockFromCandidates(vSortedByTimestamp, mapSelectedBlocks, nSelectionIntervalStop, bnStakeModifier, &pindex))
            return error("ComputeNextStakeModifier: unable to select block at round %d", nRound);
        // write the entropy bit of the selected block
        bnStakeModifierNew |= (((uint64_t)pindex->GetStakeEntropyBit()) << nRound);
        // add the selected block from candidates to selected list
        mapSelectedBlocks.insert(make_pair(pindex->GetBlockHash(), pindex));
        if (fDebug && GetBoolArg("-printstakemodifier"))
            printf("ComputeNextStakeModifier: selected round %d stop=%s height=%d bit=%d\n", nRound, DateTimeStrFormat(nSelectionIntervalStop).c_str(), pindex->nHeight, pindex->GetStakeEntropyBit());
    }

    // Print selection map for visualization of the selected blocks
    if (fDebug && GetBoolArg("-printstakemodifier"))
    {
        string strSelectionMap = "";
        // '-' indicates proof-of-work blocks not selected
        strSelectionMap.insert(0, pindexPrev->nHeight - nHeightFirstCandidate + 1, '-');
        pindex = pindexPrev;
        while (pindex && pindex->nHeight >= nHeightFirstCandidate)
        {
            // '=' indicates proof-of-stake blocks not selected
            if (pindex->IsProofOfStake())
                strSelectionMap.replace(pindex->nHeight - nHeightFirstCandidate, 1, "=");
            pindex = pindex->pprev;
        }
        BOOST_FOREACH(const PAIRTYPE(uint256, const CBlockIndex*)& item, mapSelectedBlocks)
        {
            // 'S' indicates selected proof-of-stake blocks
            // 'W' indicates selected proof-of-work blocks
            strSelectionMap.replace(item.second->nHeight - nHeightFirstCandidate, 1, item.second->IsProofOfStake()? "S" : "W");
        }
        printf("ComputeNextStakeModifier: selection height [%d, %d] map %s\n", nHeightFirstCandidate, pindexPrev->nHeight, strSelectionMap.c_str());
    }
    if (fDebug)
    {
        printf("ComputeNextStakeModifier: new modifier=%s time=%s\n", bnStakeModifierNew.ToString().c_str(), DateTimeStrFormat(pindexPrev->GetBlockTime()).c_str());
    }

    bnStakeModifier = bnStakeModifierNew;
    fGeneratedStakeModifier = true;
    return true;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
uint256 ComputeStakeModifier(const CBlockIndex* pindexPrev, const uint256& kernel)
{
    if (!pindexPrev)
        return 0;  // genesis block's modifier is 0

    CDataStream ss(SER_GETHASH, 0);
    ss << kernel << pindexPrev->bnStakeModifier;
    return Hash(ss.begin(), ss.end());
}



// ppcoin kernel protocol
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(bnStakeModifier + txPrev.block.nTime + txPrev.offset + txPrev.nTime + txPrev.vout.n + nTime) < bnTarget * nCoinDayWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coin age one owns.
// The reason this hash is chosen is the following:
//   bnStakeModifier: scrambles computation to make it very difficult to precompute
//                  future proof-of-stake at the time of the coin's confirmation
//   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
//                       generate transaction for future advantage
//   txPrev.offset: offset of txPrev inside block, to reduce the chance of 
//                  nodes generating coinstake at the same time
//   txPrev.nTime: reduce the chance of nodes generating coinstake at the same
//                 time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
bool CheckStakeKernelHash(CBlockIndex* pindexPrev, unsigned int nBits, const CBlock& blockFrom, const CTransaction& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, uint256& targetProofOfStake, bool fPrintProofOfStake)
{
    if (nTimeTx < txPrev.nTime)  // Transaction timestamp violation
        return error("CheckStakeKernelHash() : nTime violation");

    unsigned int nTimeBlockFrom = blockFrom.GetBlockTime();

    // Base target
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    // Weighted target
    int64_t nValueIn = txPrev.vout[prevout.n].nValue;
    CBigNum bnWeight = CBigNum(nValueIn);
    bnTarget *= bnWeight;

    // TODO: is ths check really necessary?
    int nColor = txPrev.vout[prevout.n].nColor;

    if (!CanStake(nColor))
    {
          return false;
    }

    targetProofOfStake = bnTarget.getuint256();

    uint256 bnStakeModifier = pindexPrev->bnStakeModifier;
    int nStakeModifierHeight = pindexPrev->nHeight;
    int64_t nStakeModifierTime = pindexPrev->nTime;

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    ss << bnStakeModifier;
    ss << txPrev.nTime << prevout.hash << prevout.n << nTimeTx;
    hashProofOfStake = Hash(ss.begin(), ss.end());
    if (fPrintProofOfStake)
    {
        printf("CheckStakeKernelHash() : using modifier %s at height=%d timestamp=%s for block from timestamp=%s\n",
            bnStakeModifier.ToString().c_str(), nStakeModifierHeight,
            DateTimeStrFormat(nStakeModifierTime).c_str(),
            DateTimeStrFormat(nTimeBlockFrom).c_str());
        printf("CheckStakeKernelHash() : check modifier=%s nTimeBlockFrom=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
            bnStakeModifier.ToString().c_str(),
            nTimeBlockFrom, txPrev.nTime, prevout.n, nTimeTx,
            hashProofOfStake.ToString().c_str());
    }

    bnTarget *= GetWeightMultiplier(nColor, pindexPrev->nTime);

    // Now check if proof-of-stake hash meets target protocol
    if (CBigNum(hashProofOfStake) > bnTarget)
    {
        return false;
    }
    
    if (fDebug && !fPrintProofOfStake)
    {
        printf("CheckStakeKernelHash() : using modifier %s at height=%d timestamp=%s for block from timestamp=%s\n",
            bnStakeModifier.ToString().c_str(), nStakeModifierHeight,
            DateTimeStrFormat(nStakeModifierTime).c_str(),
            DateTimeStrFormat(nTimeBlockFrom).c_str());
        printf("CheckStakeKernelHash() : pass modifier=%s nTimeBlockFrom=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
            bnStakeModifier.ToString().c_str(),
            nTimeBlockFrom, txPrev.nTime, prevout.n, nTimeTx,
            hashProofOfStake.ToString().c_str());
    }
    return true;

}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CBlockIndex* pindexPrev, const CTransaction& tx, unsigned int nBits, uint256& hashProofOfStake, uint256& targetProofOfStake)
{
    if (!tx.IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString().c_str());

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = tx.vin[0];

    // First try finding the previous transaction in database
    CTxDB txdb("r");
    CTransaction txPrev;
    CTxIndex txindex;
    if (!txPrev.ReadFromDisk(txdb, txin.prevout, txindex))
        return tx.DoS(1, error("CheckProofOfStake() : INFO: read txPrev failed"));  // previous transaction not in main chain, may occur during initial download

    // Verify signature
    if (!VerifySignature(txPrev, tx, 0, SCRIPT_VERIFY_NONE, 0))
        return tx.DoS(100, error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString().c_str()));

    // Read block header
    CBlock block;
    if (!block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
        return fDebug? error("CheckProofOfStake() : read block failed") : false; // unable to read block of previous transaction

    int nStakeMinConfs = GetStakeMinConfirmations(tx.GetStakeColor());
    int nDepth;
    if (IsConfirmedInNPrevBlocks(txindex, pindexPrev, nStakeMinConfs - 1, nDepth))
        return tx.DoS(100, error("CheckProofOfStake() : tried to stake at depth %d", nDepth + 1));

    if (!CheckStakeKernelHash(pindexPrev, nBits, block, txPrev, txin.prevout, tx.nTime, hashProofOfStake, targetProofOfStake, fDebug))
        return tx.DoS(1, error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s", tx.GetHash().ToString().c_str(), hashProofOfStake.ToString().c_str())); // may occur during initial download or if behind on block chain sync

    return true;
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx)
{
    int nStakeTimestampMask = GetStakeTimestampMask();
    return (nTimeBlock == nTimeTx) && ((nTimeTx & nStakeTimestampMask) == 0);
}


bool CheckKernel(int nStakeColor, CBlockIndex* pindexPrev, unsigned int nBits,
                 int64_t nTime, const COutPoint& prevout, int64_t* pBlockTime)
{
    uint256 hashProofOfStake, targetProofOfStake;

    CTxDB txdb("r");
    CTransaction txPrev;
    CTxIndex txindex;
    if (!txPrev.ReadFromDisk(txdb, prevout, txindex))
        return false;

    // Read block header
    CBlock block;
    if (!block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
        return false;

    int nStakeMinConfs = GetStakeMinConfirmations(nStakeColor);

    int nDepth;
    if (IsConfirmedInNPrevBlocks(txindex, pindexPrev, nStakeMinConfs - 1, nDepth))
        return false;

    if (pBlockTime)
        *pBlockTime = block.GetBlockTime();

    return CheckStakeKernelHash(pindexPrev, nBits, block, txPrev, prevout, nTime, hashProofOfStake, targetProofOfStake);
}

// Get stake modifier checksum
unsigned int GetStakeModifierChecksum(const CBlockIndex* pindex)
{
    if (!pindex->pprev && fDebug) {
           printf("GetStakeModifierChecksum:\n");
           printf("   real genesis: %s\n   expected: %s\n", pindex->GetBlockHash().ToString().c_str(),
                       (!fTestNet ? hashGenesisBlock.ToString().c_str() :
                                    hashGenesisBlockTestNet.ToString().c_str()));
    }
    if (fDebug) {
        printf("GetStakeModifierChecksum: block %s\n", pindex->GetBlockHash().ToString().c_str());
    }
    assert (pindex->pprev || pindex->GetBlockHash() == (!fTestNet ? hashGenesisBlock : hashGenesisBlockTestNet));
    // Hash previous checksum with flags, hashProofOfStake and bnStakeModifier
    CDataStream ss(SER_GETHASH, 0);
    if (pindex->pprev)
        ss << pindex->pprev->nStakeModifierChecksum;
    ss << pindex->nFlags << (pindex->IsProofOfStake() ? pindex->hashProof : 0) << pindex->bnStakeModifier;
    uint256 hashChecksum = Hash(ss.begin(), ss.end());
    hashChecksum >>= (256 - 32);
    return hashChecksum.Get64();
}

// Check stake modifier hard checkpoints
bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum)
{
    MapModifierCheckpoints& checkpoints = (fTestNet ? mapStakeModifierCheckpointsTestNet : mapStakeModifierCheckpoints);

    if (checkpoints.count(nHeight))
        return nStakeModifierChecksum == checkpoints[nHeight];
    return true;
}
