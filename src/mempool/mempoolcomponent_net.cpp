#include "mempoolcomponent.h"
#include "txmempool.h"
#include "sbtccore/clientversion.h"
#include "config/consensus.h"
#include "config/argmanager.h"
#include "chaincontrol/validation.h"
#include "block/validation.h"
#include "sbtccore/transaction/policy.h"
#include "sbtccore/block/blockencodings.h"
#include "wallet/fees.h"
#include "reverse_iterator.h"
#include "sbtccore/streams.h"
#include "timedata.h"
#include "utils/util.h"
#include "utils/utilmoneystr.h"
#include "utils/utiltime.h"
#include "net_processing.h"
#include "wallet/rbf.h"
#include "sbtcd/baseimpl.hpp"
#include "interface/inetcomponent.h"
#include "interface/ichaincomponent.h"
#include "utils/net/netmessagehelper.h"
#include "chaincontrol/utils.h"

SET_CPP_SCOPED_LOG_CATEGORY(CID_TX_MEMPOOL);

namespace
{
    /**
     * Filter for transactions that were recently rejected by
     * AcceptToMemoryPool. These are not rerequested until the chain tip
     * changes, at which point the entire filter is reset. Protected by
     * cs_main.
     *
     * Without this filter we'd be re-requesting txs from each of our peers,
     * increasing bandwidth consumption considerably. For instance, with 100
     * peers, half of which relay a tx we don't accept, that might be a 50x
     * bandwidth increase. A flooding attacker attempting to roll-over the
     * filter using minimum-sized, 60byte, transactions might manage to send
     * 1000/sec if we have fast peers, so we pick 120,000 to give our peers a
     * two minute window to send invs to us.
     *
     * Decreasing the false positive rate is fairly cheap, so we pick one in a
     * million to make it highly unlikely for users to have issues with this
     * filter.
     *
     * Memory used: 1.3 MB
     */
    std::unique_ptr<CRollingBloomFilter> recentRejects;
    uint256 hashRecentRejectsChainTip;


    /** Relay map, protected by cs_main. */
    typedef std::map<uint256, CTransactionRef> MapRelay;

    MapRelay mapRelay;

    /** Expiration-time ordered list of (expire time, relay map entry) pairs, protected by cs_main). */
    std::deque<std::pair<int64_t, MapRelay::iterator>> vRelayExpiration;
}

void CMempoolComponent::InitializeForNet()
{
    // Initialize global variables that cannot be constructed at startup.
    recentRejects.reset(new CRollingBloomFilter(120000, 0.000001));
}

bool CMempoolComponent::DoesTxExist(uint256 txHash)
{
    GET_CHAIN_INTERFACE(ifChainObj);
    if (recentRejects)
    {
        CChain &chainActive = ifChainObj->GetActiveChain();
        uint256 tipBlockHash = chainActive.Tip()->GetBlockHash();
        if (tipBlockHash != hashRecentRejectsChainTip)
        {
            // If the chain tip has changed previously rejected transactions
            // might be now valid, e.g. due to a nLockTime'd tx becoming valid,
            // or a double-spend. Reset the rejects filter and give those
            // txs a second chance.
            hashRecentRejectsChainTip = tipBlockHash;
            recentRejects->reset();
        }
        else if (recentRejects->contains(txHash))
            return true;
    }

    if (mempool.exists(txHash) || orphanTxMgr.Exists(txHash))
        return true;

    if (CCoinsViewCache *pcoinsTip = ifChainObj->GetCoinsTip())
        return pcoinsTip->HaveCoinInCache(COutPoint(txHash, 0)) || // Best effort: only try output 0 and 1
               pcoinsTip->HaveCoinInCache(COutPoint(txHash, 1));

    return false;
}

bool CMempoolComponent::NetRequestTxData(ExNode *xnode, uint256 txHash, bool witness, int64_t timeLastMempoolReq)
{
    assert(xnode != nullptr);

    int nSendFlags = witness ? 0 : SERIALIZE_TRANSACTION_NO_WITNESS;

    auto mi = mapRelay.find(txHash);
    if (mi != mapRelay.end())
    {
        SendNetMessage(xnode->nodeID, NetMsgType::TX, xnode->sendVersion, nSendFlags, *mi->second);
        return true;
    }

    auto txinfo = GetMemPool().info(txHash);
    if (txinfo.tx && txinfo.nTime <= timeLastMempoolReq)
    {
        SendNetMessage(xnode->nodeID, NetMsgType::TX, xnode->sendVersion, nSendFlags, *txinfo.tx);
        return true;
    }

    return false;
}

bool CMempoolComponent::NetReceiveTxData(ExNode *xnode, CDataStream &stream, uint256 &txHash)
{
    assert(xnode != nullptr);

    // Stop processing the transaction early if
    // We are in blocks only mode and peer is either not whitelisted or whitelistrelay is off
    if (!IsFlagsBitOn(xnode->flags, NF_RELAYTX) &&
        (!IsFlagsBitOn(xnode->flags, NF_WHITELIST) ||
         !Args().GetArg<bool>("-whitelistrelay", DEFAULT_WHITELISTRELAY)))
    {
        NLogFormat("transaction sent in violation of protocol peer=%d", xnode->nodeID);
        return true;
    }

    GET_NET_INTERFACE(ifNetObj);
    assert(ifNetObj != nullptr);

    CTransactionRef ptx;
    stream >> ptx;
    const CTransaction &tx = *ptx;

    txHash = tx.GetHash();
    CInv inv(MSG_TX, txHash);

    std::deque<COutPoint> vWorkQueue;
    std::vector<uint256> vEraseQueue;


    bool fMissingInputs = false;
    CValidationState state;

    std::list<CTransactionRef> lRemovedTxn;

    if (!DoesTxExist(tx.GetHash()) &&
        GetMemPool().AcceptToMemoryPool(state, ptx, true, &fMissingInputs, &lRemovedTxn))
    {
        GET_CHAIN_INTERFACE(ifChainObj);
        CCoinsViewCache *pcoinsTip = ifChainObj->GetCoinsTip();
        GetMemPool().Check(pcoinsTip);
        ifNetObj->BroadcastTransaction(tx.GetHash());

        for (unsigned int i = 0; i < tx.vout.size(); i++)
        {
            vWorkQueue.emplace_back(inv.hash, i);
        }

        SetFlagsBit(xnode->retFlags, NF_NEWTRANSACTION);

        NLogFormat("AcceptToMemoryPool: peer=%d: accepted %s (poolsz %u txn, %u kB)",
                    xnode->nodeID,
                    tx.GetHash().ToString(),
                    GetMemPool().size(), GetMemPool().DynamicMemoryUsage() / 1000);

        // Recursively process any orphan transactions that depended on this one
        std::set<NodeId> setMisbehaving;
        while (!vWorkQueue.empty())
        {
            ITBYPREV itByPrev;
            int ret = orphanTxMgr.FindOrphanTransactionsByPrev(vWorkQueue.front(), itByPrev);
            vWorkQueue.pop_front();
            if (ret == 0)
                continue;
            for (auto mi = itByPrev->second.begin();
                 mi != itByPrev->second.end();
                 ++mi)
            {
                const CTransactionRef &porphanTx = (*mi)->second.tx;
                const CTransaction &orphanTx = *porphanTx;
                const uint256 &orphanHash = orphanTx.GetHash();
                NodeId fromPeer = (*mi)->second.fromPeer;
                bool fMissingInputs2 = false;
                // Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan
                // resolution (that is, feeding people an invalid transaction based on LegitTxX in order to get
                // anyone relaying LegitTxX banned)
                CValidationState stateDummy;


                if (setMisbehaving.count(fromPeer))
                    continue;
                if (GetMemPool().AcceptToMemoryPool(stateDummy, porphanTx, true, &fMissingInputs2, &lRemovedTxn))
                {
                    NLogFormat("accepted orphan tx %s", orphanHash.ToString());

                    ifNetObj->BroadcastTransaction(orphanTx.GetHash());

                    for (unsigned int i = 0; i < orphanTx.vout.size(); i++)
                    {
                        vWorkQueue.emplace_back(orphanHash, i);
                    }

                    vEraseQueue.push_back(orphanHash);
                } else if (!fMissingInputs2)
                {
                    int nDos = 0;
                    if (stateDummy.IsInvalid(nDos) && nDos > 0)
                    {
                        // Punish peer that gave us an invalid orphan tx
                        ifNetObj->MisbehaveNode(fromPeer, nDos);
                        setMisbehaving.insert(fromPeer);
                        NLogFormat("invalid orphan tx %s", orphanHash.ToString());
                    }
                    // Has inputs but not accepted to mempool
                    // Probably non-standard or insufficient fee
                    NLogFormat("removed orphan tx %s", orphanHash.ToString());
                    vEraseQueue.push_back(orphanHash);
                    if (!orphanTx.HasWitness() && !stateDummy.CorruptionPossible())
                    {
                        // Do not use rejection cache for witness transactions or
                        // witness-stripped transactions, as they can have been malleated.
                        // See https://github.com/bitcoin/bitcoin/issues/8279 for details.
                        assert(recentRejects);
                        recentRejects->insert(orphanHash);
                    }
                }
                GetMemPool().Check(pcoinsTip);
            }
        }

        for (uint256 hash : vEraseQueue)
            orphanTxMgr.EraseOrphanTx(hash);
    } else if (fMissingInputs)
    {
        bool fRejectedParents = false; // It may be the case that the orphans parents have all been rejected
        for (const CTxIn &txin : tx.vin)
        {
            if (recentRejects->contains(txin.prevout.hash))
            {
                fRejectedParents = true;
                break;
            }
        }

        if (!fRejectedParents)
        {
            uint32_t nFetchFlags = 0;
            if (IsFlagsBitOn(xnode->nLocalServices, NODE_WITNESS) &&
                IsFlagsBitOn(xnode->flags, NF_WITNESS))
                nFetchFlags = MSG_WITNESS_FLAG;

            for (const CTxIn &txin : tx.vin)
            {
                CInv _inv(MSG_TX | nFetchFlags, txin.prevout.hash);
                ifNetObj->AddTxInventoryKnown(xnode->nodeID, _inv.hash, nFetchFlags);
                if (!DoesTxExist(_inv.hash))
                    ifNetObj->AskForTransaction(xnode->nodeID, _inv.hash, nFetchFlags);
            }

            orphanTxMgr.AddOrphanTx(ptx, xnode->nodeID);

            // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
            unsigned int nMaxOrphanTx = (unsigned int)std::max((int64_t)0,
                                                               int64_t(Args().GetArg<uint32_t>("-maxorphantx",
                                                                                               DEFAULT_MAX_ORPHAN_TRANSACTIONS)));
            unsigned int nEvicted = orphanTxMgr.LimitOrphanTxSize(nMaxOrphanTx);
            if (nEvicted > 0)
            {
                NLogFormat("mapOrphan overflow, removed %u tx", nEvicted);
            }
        } else
        {
            NLogFormat("not keeping orphan with rejected parents %s", tx.GetHash().ToString());
            // We will continue to reject this tx since it has rejected
            // parents so avoid re-requesting it from other peers.
            recentRejects->insert(tx.GetHash());
        }
    } else
    {
        if (!tx.HasWitness() && !state.CorruptionPossible())
        {
            // Do not use rejection cache for witness transactions or
            // witness-stripped transactions, as they can have been malleated.
            // See https://github.com/bitcoin/bitcoin/issues/8279 for details.
            assert(recentRejects);
            recentRejects->insert(tx.GetHash());
            if (RecursiveDynamicUsage(*ptx) < 100000)
            {
                AddToCompactExtraTransactions(ptx);
            }
        } else if (tx.HasWitness() && RecursiveDynamicUsage(*ptx) < 100000)
        {
            AddToCompactExtraTransactions(ptx);
        }

        if (IsFlagsBitOn(xnode->flags, NF_WHITELIST) &&
            Args().GetArg<bool>("-whitelistforcerelay", DEFAULT_WHITELISTFORCERELAY))
        {
            // Always relay transactions received from whitelisted peers, even
            // if they were already in the mempool or rejected from it due
            // to policy, allowing the node to function as a gateway for
            // nodes hidden behind it.
            //
            // Never relay transactions that we would assign a non-zero DoS
            // score for, as we expect peers to do the same with us in that
            // case.
            int nDoS = 0;
            if (!state.IsInvalid(nDoS) || nDoS == 0)
            {
                NLogFormat("Force relaying tx %s from whitelisted peer=%d", tx.GetHash().ToString(),
                            xnode->nodeID);

                ifNetObj->BroadcastTransaction(tx.GetHash());
            } else
            {
                NLogFormat("Not relaying invalid transaction %s from whitelisted peer=%d (%s)",
                            tx.GetHash().ToString(), xnode->nodeID, FormatStateMessage(state));
            }
        }
    }

    for (const CTransactionRef &removedTx : lRemovedTxn)
        AddToCompactExtraTransactions(removedTx);

    int nDoS = 0;
    if (state.IsInvalid(nDoS))
    {
        ELogFormat("%s from peer=%d was not accepted: %s", tx.GetHash().ToString(),
                   xnode->nodeID,
                   FormatStateMessage(state));
        if (state.GetRejectCode() > 0 &&
            state.GetRejectCode() < REJECT_INTERNAL) // Never send AcceptToMemoryPool's internal codes over P2P
        {
            SendNetMessage(xnode->nodeID, NetMsgType::REJECT, xnode->sendVersion, 0,
                           std::string(NetMsgType::TX),
                           (unsigned char)state.GetRejectCode(),
                           state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH),
                           inv.hash);
        }


        if (nDoS > 0)
        {
            xnode->nMisbehavior = nDoS;
        }
    }
    return true;
}

bool
CMempoolComponent::NetRequestTxInventory(ExNode *xnode, bool sendMempool, int64_t minFeeFilter, CBloomFilter *txFilter,
                                         std::vector<uint256> &toSendTxHashes, std::vector<uint256> &haveSentTxHashes)
{
    assert(xnode != nullptr);

    GET_NET_INTERFACE(ifNetObj);
    assert(ifNetObj != nullptr);

    std::vector<CInv> vInv;
    if (sendMempool)
    {
        auto vtxinfo = GetMemPool().infoAll();
        for (const auto &txinfo : vtxinfo)
        {
            const uint256 &hash = txinfo.tx->GetHash();
            CInv inv(MSG_TX, hash);

            auto it = std::find(toSendTxHashes.begin(), toSendTxHashes.end(), hash);
            if (it != toSendTxHashes.end())
                toSendTxHashes.erase(it);

            if (txinfo.feeRate.GetFeePerK() < minFeeFilter)
                continue;

            if (txFilter)
                if (!txFilter->IsRelevantAndUpdate(*txinfo.tx))
                    continue;

            haveSentTxHashes.emplace_back(hash);
            vInv.push_back(inv);
            if (vInv.size() == MAX_INV_SZ)
            {
                SendNetMessage(xnode->nodeID, NetMsgType::INV, xnode->sendVersion, 0, vInv);
                vInv.clear();
            }
        }
    }

    if (!toSendTxHashes.empty())
    {
        int64_t nNow = GetTimeMicros();

        // Expire old relay messages
        while (!vRelayExpiration.empty() && vRelayExpiration.front().first < nNow)
        {
            mapRelay.erase(vRelayExpiration.front().second);
            vRelayExpiration.pop_front();
        }

        // Topologically and fee-rate sort the inventory we send for privacy and priority reasons.
        // A heap is used so that not all items need sorting if only a few are being sent.
        auto compFunc = [this](uint256 a, uint256 b)
        { return GetMemPool().CompareDepthAndScore(b, a); };
        std::make_heap(toSendTxHashes.begin(), toSendTxHashes.end(), compFunc);

        // No reason to drain out at many times the network's capacity,
        // especially since we have many peers and some will draw much shorter delays.
        unsigned int nRelayedTransactions = 0;

        while (!toSendTxHashes.empty() && nRelayedTransactions < INVENTORY_BROADCAST_MAX)
        {
            // Fetch the top element from the heap
            std::pop_heap(toSendTxHashes.begin(), toSendTxHashes.end(), compFunc);

            uint256 hash = toSendTxHashes.back();
            toSendTxHashes.pop_back();

            auto txinfo = GetMemPool().info(hash);
            if (!txinfo.tx)
                continue;

            if (txinfo.feeRate.GetFeePerK() < minFeeFilter)
                continue;

            if (txFilter)
                if (!txFilter->IsRelevantAndUpdate(*txinfo.tx))
                    continue;

            haveSentTxHashes.emplace_back(hash);

            auto ret = mapRelay.insert(std::make_pair(hash, std::move(txinfo.tx)));
            if (ret.second)
            {
                vRelayExpiration.push_back(std::make_pair(nNow + 15 * 60 * 1000000, ret.first));
            }

            vInv.push_back(CInv(MSG_TX, hash));
            nRelayedTransactions++;

            if (vInv.size() == MAX_INV_SZ)
            {
                SendNetMessage(xnode->nodeID, NetMsgType::INV, xnode->sendVersion, 0, vInv);
                vInv.clear();
            }
        }
    }

    if (!vInv.empty())
    {
        SendNetMessage(xnode->nodeID, NetMsgType::INV, xnode->sendVersion, 0, vInv);
    }

    return true;
}

bool CMempoolComponent::RemoveOrphanTxForNode(int64_t nodeId)
{
    return orphanTxMgr.EraseOrphansFor(nodeId) > 0;
}

bool CMempoolComponent::RemoveOrphanTxForBlock(const CBlock* pblock)
{
    if (!pblock)
        return false;

    std::vector<uint256> vOrphanErase;
    for (const CTransactionRef &ptx : pblock->vtx)
    {
        const CTransaction &tx = *ptx;

        // Which orphan pool entries must we evict?
        for (const auto &txin : tx.vin)
        {
            ITBYPREV itByPrev;
            int ret = orphanTxMgr.FindOrphanTransactionsByPrev(txin.prevout, itByPrev);
            if (ret == 0)
                continue;
            for (auto mi = itByPrev->second.begin(); mi != itByPrev->second.end(); ++mi)
            {
                const CTransaction &orphanTx = *(*mi)->second.tx;
                const uint256 &orphanHash = orphanTx.GetHash();
                vOrphanErase.push_back(orphanHash);
            }
        }
    }

    // Erase orphan transactions include or precluded by this block
    if (!vOrphanErase.empty())
    {
        int nErased = 0;
        for (uint256 &orphanHash : vOrphanErase)
        {
            nErased += orphanTxMgr.EraseOrphanTx(orphanHash);
        }
        NLogFormat("Erased %d orphan tx included or conflicted by block", nErased);
        return true;
    }

    return false;
}
