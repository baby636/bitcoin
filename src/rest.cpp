// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <blockfilter.h>
#include <chain.h>
#include <chainparams.h>
#include <core_io.h>
#include <httpserver.h>
#include <index/blockfilterindex.h>
#include <index/txindex.h>
#include <node/context.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <rpc/blockchain.h>
#include <rpc/protocol.h>
#include <rpc/server.h>
#include <streams.h>
#include <sync.h>
#include <txmempool.h>
#include <util/check.h>
#include <util/fees.h>
#include <util/strencodings.h>
#include <validation.h>
#include <version.h>
#include <policy/fees.h>

#include <boost/algorithm/string.hpp>

#include <univalue.h>

static const size_t MAX_GETUTXOS_OUTPOINTS = 15; //allow a max of 15 outpoints to be queried at once

enum class RetFormat {
    UNDEF,
    BINARY,
    HEX,
    JSON,
};

static const struct {
    RetFormat rf;
    const char* name;
} rf_names[] = {
      {RetFormat::UNDEF, ""},
      {RetFormat::BINARY, "bin"},
      {RetFormat::HEX, "hex"},
      {RetFormat::JSON, "json"},
};

struct CCoin {
    uint32_t nHeight;
    CTxOut out;

    ADD_SERIALIZE_METHODS;

    CCoin() : nHeight(0) {}
    explicit CCoin(Coin&& in) : nHeight(in.nHeight), out(std::move(in.out)) {}

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        uint32_t nTxVerDummy = 0;
        READWRITE(nTxVerDummy);
        READWRITE(nHeight);
        READWRITE(out);
    }
};

static bool RESTERR(HTTPRequest* req, enum HTTPStatusCode status, std::string message)
{
    req->WriteHeader("Content-Type", "text/plain");
    req->WriteReply(status, message + "\r\n");
    return false;
}

/**
 * Get the node context mempool.
 *
 * Set the HTTP error and return nullptr if node context
 * mempool is not found.
 *
 * @param[in]  req the HTTP request
 * return pointer to the mempool or nullptr if no mempool found
 */
static CTxMemPool* GetMemPool(HTTPRequest* req)
{
    if (!g_rpc_node || !g_rpc_node->mempool) {
        RESTERR(req, HTTP_NOT_FOUND, "Mempool disabled or instance not found");
        return nullptr;
    }
    return g_rpc_node->mempool;
}

static RetFormat ParseDataFormat(std::string& param, const std::string& strReq)
{
    const std::string::size_type pos = strReq.rfind('.');
    if (pos == std::string::npos)
    {
        param = strReq;
        return rf_names[0].rf;
    }

    param = strReq.substr(0, pos);
    const std::string suff(strReq, pos + 1);

    for (unsigned int i = 0; i < ARRAYLEN(rf_names); i++)
        if (suff == rf_names[i].name)
            return rf_names[i].rf;

    /* If no suffix is found, return original string.  */
    param = strReq;
    return rf_names[0].rf;
}

static std::string AvailableDataFormatsString()
{
    std::string formats;
    for (unsigned int i = 0; i < ARRAYLEN(rf_names); i++)
        if (strlen(rf_names[i].name) > 0) {
            formats.append(".");
            formats.append(rf_names[i].name);
            formats.append(", ");
        }

    if (formats.length() > 0)
        return formats.substr(0, formats.length() - 2);

    return formats;
}

static bool CheckWarmup(HTTPRequest* req)
{
    std::string statusmessage;
    if (RPCIsInWarmup(&statusmessage))
         return RESTERR(req, HTTP_SERVICE_UNAVAILABLE, "Service temporarily unavailable: " + statusmessage);
    return true;
}

static bool rest_headers(HTTPRequest* req,
                         const std::string& strURIPart)
{
    if (!CheckWarmup(req))
        return false;
    std::string param;
    const RetFormat rf = ParseDataFormat(param, strURIPart);
    std::vector<std::string> path;
    boost::split(path, param, boost::is_any_of("/"));

    if (path.size() != 2)
        return RESTERR(req, HTTP_BAD_REQUEST, "No header count specified. Use /rest/headers/<count>/<hash>.<ext>.");

    long count = strtol(path[0].c_str(), nullptr, 10);
    if (count < 1 || count > 2000)
        return RESTERR(req, HTTP_BAD_REQUEST, "Header count out of range: " + path[0]);

    std::string hashStr = path[1];
    uint256 hash;
    if (!ParseHashStr(hashStr, hash))
        return RESTERR(req, HTTP_BAD_REQUEST, "Invalid hash: " + hashStr);

    const CBlockIndex* tip = nullptr;
    std::vector<const CBlockIndex *> headers;
    headers.reserve(count);
    {
        LOCK(cs_main);
        tip = ::ChainActive().Tip();
        const CBlockIndex* pindex = LookupBlockIndex(hash);
        while (pindex != nullptr && ::ChainActive().Contains(pindex)) {
            headers.push_back(pindex);
            if (headers.size() == (unsigned long)count)
                break;
            pindex = ::ChainActive().Next(pindex);
        }
    }

    switch (rf) {
    case RetFormat::BINARY: {
        CDataStream ssHeader(SER_NETWORK, PROTOCOL_VERSION);
        for (const CBlockIndex *pindex : headers) {
            ssHeader << pindex->GetBlockHeader();
        }

        std::string binaryHeader = ssHeader.str();
        req->WriteHeader("Content-Type", "application/octet-stream");
        req->WriteReply(HTTP_OK, binaryHeader);
        return true;
    }

    case RetFormat::HEX: {
        CDataStream ssHeader(SER_NETWORK, PROTOCOL_VERSION);
        for (const CBlockIndex *pindex : headers) {
            ssHeader << pindex->GetBlockHeader();
        }

        std::string strHex = HexStr(ssHeader.begin(), ssHeader.end()) + "\n";
        req->WriteHeader("Content-Type", "text/plain");
        req->WriteReply(HTTP_OK, strHex);
        return true;
    }
    case RetFormat::JSON: {
        UniValue jsonHeaders(UniValue::VARR);
        for (const CBlockIndex *pindex : headers) {
            jsonHeaders.push_back(blockheaderToJSON(tip, pindex));
        }
        std::string strJSON = jsonHeaders.write() + "\n";
        req->WriteHeader("Content-Type", "application/json");
        req->WriteReply(HTTP_OK, strJSON);
        return true;
    }
    default: {
        return RESTERR(req, HTTP_NOT_FOUND, "output format not found (available: .bin, .hex)");
    }
    }
}

static bool rest_block(HTTPRequest* req,
                       const std::string& strURIPart,
                       bool showTxDetails)
{
    if (!CheckWarmup(req))
        return false;
    std::string hashStr;
    const RetFormat rf = ParseDataFormat(hashStr, strURIPart);

    uint256 hash;
    if (!ParseHashStr(hashStr, hash))
        return RESTERR(req, HTTP_BAD_REQUEST, "Invalid hash: " + hashStr);

    CBlock block;
    CBlockIndex* pblockindex = nullptr;
    CBlockIndex* tip = nullptr;
    {
        LOCK(cs_main);
        tip = ::ChainActive().Tip();
        pblockindex = LookupBlockIndex(hash);
        if (!pblockindex) {
            return RESTERR(req, HTTP_NOT_FOUND, hashStr + " not found");
        }

        if (IsBlockPruned(pblockindex))
            return RESTERR(req, HTTP_NOT_FOUND, hashStr + " not available (pruned data)");

        if (!ReadBlockFromDisk(block, pblockindex, Params().GetConsensus()))
            return RESTERR(req, HTTP_NOT_FOUND, hashStr + " not found");
    }

    switch (rf) {
    case RetFormat::BINARY: {
        CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION | RPCSerializationFlags());
        ssBlock << block;
        std::string binaryBlock = ssBlock.str();
        req->WriteHeader("Content-Type", "application/octet-stream");
        req->WriteReply(HTTP_OK, binaryBlock);
        return true;
    }

    case RetFormat::HEX: {
        CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION | RPCSerializationFlags());
        ssBlock << block;
        std::string strHex = HexStr(ssBlock.begin(), ssBlock.end()) + "\n";
        req->WriteHeader("Content-Type", "text/plain");
        req->WriteReply(HTTP_OK, strHex);
        return true;
    }

    case RetFormat::JSON: {
        UniValue objBlock = blockToJSON(block, tip, pblockindex, showTxDetails);
        std::string strJSON = objBlock.write() + "\n";
        req->WriteHeader("Content-Type", "application/json");
        req->WriteReply(HTTP_OK, strJSON);
        return true;
    }

    default: {
        return RESTERR(req, HTTP_NOT_FOUND, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }
}

static bool rest_block_extended(HTTPRequest* req, const std::string& strURIPart)
{
    return rest_block(req, strURIPart, true);
}

static bool rest_block_notxdetails(HTTPRequest* req, const std::string& strURIPart)
{
    return rest_block(req, strURIPart, false);
}

static bool rest_filter_header(HTTPRequest* req, const std::string& strURIPart)
{
    if (!CheckWarmup(req))
        return false;
    std::string param;
    const RetFormat rf = ParseDataFormat(param, strURIPart);

    std::vector<std::string> uriParts;
    boost::split(uriParts, param, boost::is_any_of("/"));
    if (uriParts.size() != 3) {
        return RESTERR(req, HTTP_BAD_REQUEST, "Invalid URI format. Expected /rest/blockfilterheaders/filtertype/count/blockhash");
    }

    uint256 block_hash;
    if (!ParseHashStr(uriParts[2], block_hash)) {
        return RESTERR(req, HTTP_BAD_REQUEST, "Invalid hash: " + uriParts[2]);
    }

    BlockFilterType filtertype;
    if (!BlockFilterTypeByName(uriParts[0], filtertype)) {
        return RESTERR(req, HTTP_BAD_REQUEST, "Unknown filtertype " + uriParts[0]);
    }

    BlockFilterIndex* index = GetBlockFilterIndex(filtertype);
    if (!index) {
        return RESTERR(req, HTTP_BAD_REQUEST, "Index is not enabled for filtertype " + uriParts[0]);
    }

    long count = strtol(uriParts[1].c_str(), nullptr, 10);
    if (count < 1 || count > MAX_HEADERS_RESULTS) {
        return RESTERR(req, HTTP_BAD_REQUEST, strprintf("Header count out of acceptable range (1-%u): %s",  MAX_HEADERS_RESULTS, uriParts[1]));
    }

    std::vector<const CBlockIndex *> headers;
    headers.reserve(count);
    {
        LOCK(cs_main);
        const CBlockIndex* pindex = LookupBlockIndex(block_hash);
        while (pindex != nullptr && ::ChainActive().Contains(pindex)) {
            headers.push_back(pindex);
            if (headers.size() == (unsigned long)count)
                break;
            pindex = ::ChainActive().Next(pindex);
        }
    }

    bool index_ready = index->BlockUntilSyncedToCurrentChain();

    std::vector<uint256> filter_headers;
    filter_headers.reserve(count);
    for (const CBlockIndex *pindex : headers) {
        uint256 filter_header;
        if (!index->LookupFilterHeader(pindex, filter_header)) {
            std::string errmsg = "Filter not found.";

            if (!index_ready) {
                errmsg += " Block filters are still in the process of being indexed.";
            } else {
                errmsg += " This error is unexpected and indicates index corruption.";
            }

            return RESTERR(req, HTTP_NOT_FOUND, errmsg);
        }
        filter_headers.push_back(filter_header);
    }

    switch (rf) {
    case RetFormat::BINARY: {
        CDataStream ssHeader(SER_NETWORK, PROTOCOL_VERSION);
        for (const uint256& header : filter_headers) {
            ssHeader << header;
        }

        std::string binaryHeader = ssHeader.str();
        req->WriteHeader("Content-Type", "application/octet-stream");
        req->WriteReply(HTTP_OK, binaryHeader);
        return true;
    }

    case RetFormat::HEX: {
        CDataStream ssHeader(SER_NETWORK, PROTOCOL_VERSION);
        for (const uint256& header : filter_headers) {
            ssHeader << header;
        }

        std::string strHex = HexStr(ssHeader.begin(), ssHeader.end()) + "\n";
        req->WriteHeader("Content-Type", "text/plain");
        req->WriteReply(HTTP_OK, strHex);
        return true;
    }
    case RetFormat::JSON: {
        UniValue jsonHeaders(UniValue::VARR);
        for (const uint256& header : filter_headers) {
            jsonHeaders.push_back(header.GetHex());
        }

        std::string strJSON = jsonHeaders.write() + "\n";
        req->WriteHeader("Content-Type", "application/json");
        req->WriteReply(HTTP_OK, strJSON);
        return true;
    }
    default: {
        return RESTERR(req, HTTP_NOT_FOUND, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }

}
static bool rest_block_filter(HTTPRequest* req, const std::string& strURIPart)
{
    if (!CheckWarmup(req))
        return false;
    std::string param;
    const RetFormat rf = ParseDataFormat(param, strURIPart);

    //request is sent over URI scheme /rest/blockfilter/filtertype/blockhash
    std::vector<std::string> uriParts;
    boost::split(uriParts, param, boost::is_any_of("/"));
    if (uriParts.size() != 2) {
        return RESTERR(req, HTTP_BAD_REQUEST, "Invalid URI format. Expected /rest/blockfilter/filtertype/blockhash");
    }

    uint256 block_hash;
    if (!ParseHashStr(uriParts[1], block_hash)) {
        return RESTERR(req, HTTP_BAD_REQUEST, "Invalid hash: " + uriParts[1]);
    }

    BlockFilterType filtertype;
    if (!BlockFilterTypeByName(uriParts[0], filtertype)) {
        return RESTERR(req, HTTP_BAD_REQUEST, "Unknown filtertype " + uriParts[0]);
    }

    BlockFilterIndex* index = GetBlockFilterIndex(filtertype);
    if (!index) {
        return RESTERR(req, HTTP_BAD_REQUEST, "Index is not enabled for filtertype " + uriParts[0]);
    }

    const CBlockIndex* block_index;
    bool block_was_connected;
    {
        LOCK(cs_main);
        block_index = LookupBlockIndex(block_hash);
        if (!block_index) {
            return RESTERR(req, HTTP_NOT_FOUND, uriParts[1] + " not found");
        }
        block_was_connected = block_index->IsValid(BLOCK_VALID_SCRIPTS);
    }

    bool index_ready = index->BlockUntilSyncedToCurrentChain();

    BlockFilter filter;
    if (!index->LookupFilter(block_index, filter)) {
        std::string errmsg = "Filter not found.";

        if (!block_was_connected) {
            errmsg += " Block was not connected to active chain.";
        } else if (!index_ready) {
            errmsg += " Block filters are still in the process of being indexed.";
        } else {
            errmsg += " This error is unexpected and indicates index corruption.";
        }

        return RESTERR(req, HTTP_NOT_FOUND, errmsg);
    }

    switch (rf) {
    case RetFormat::BINARY: {
        CDataStream ssResp(SER_NETWORK, PROTOCOL_VERSION | RPCSerializationFlags());
        ssResp << filter;

        std::string binaryResp = ssResp.str();
        req->WriteHeader("Content-Type", "application/octet-stream");
        req->WriteReply(HTTP_OK, binaryResp);
        return true;
    }

    case RetFormat::HEX: {
        CDataStream ssResp(SER_NETWORK, PROTOCOL_VERSION | RPCSerializationFlags());
        ssResp << filter;

        std::string strHex = HexStr(ssResp.begin(), ssResp.end()) + "\n";
        req->WriteHeader("Content-Type", "text/plain");
        req->WriteReply(HTTP_OK, strHex);
        return true;
    }

    case RetFormat::JSON: {
        UniValue ret(UniValue::VOBJ);
        ret.pushKV("filter", HexStr(filter.GetEncodedFilter()));
        std::string strJSON = ret.write() + "\n";
        req->WriteHeader("Content-Type", "application/json");
        req->WriteReply(HTTP_OK, strJSON);
        return true;
    }

    default: {
        return RESTERR(req, HTTP_NOT_FOUND, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }
}

// A bit of a hack - dependency on a function defined in rpc/blockchain.cpp
UniValue getblockchaininfo(const JSONRPCRequest& request);

static bool rest_chaininfo(HTTPRequest* req, const std::string& strURIPart)
{
    if (!CheckWarmup(req))
        return false;
    std::string param;
    const RetFormat rf = ParseDataFormat(param, strURIPart);

    switch (rf) {
    case RetFormat::JSON: {
        JSONRPCRequest jsonRequest;
        jsonRequest.params = UniValue(UniValue::VARR);
        UniValue chainInfoObject = getblockchaininfo(jsonRequest);
        std::string strJSON = chainInfoObject.write() + "\n";
        req->WriteHeader("Content-Type", "application/json");
        req->WriteReply(HTTP_OK, strJSON);
        return true;
    }
    default: {
        return RESTERR(req, HTTP_NOT_FOUND, "output format not found (available: json)");
    }
    }
}

static bool rest_mempool_info(HTTPRequest* req, const std::string& strURIPart, bool fee_histogram)
{
    if (!CheckWarmup(req))
        return false;
    const CTxMemPool* mempool = GetMemPool(req);
    if (!mempool) return false;
    std::string param;
    const RetFormat rf = ParseDataFormat(param, strURIPart);

    switch (rf) {
    case RetFormat::JSON: {
        UniValue mempoolInfoObject = MempoolInfoToJSON(*mempool, fee_histogram);

        std::string strJSON = mempoolInfoObject.write() + "\n";
        req->WriteHeader("Content-Type", "application/json");
        req->WriteReply(HTTP_OK, strJSON);
        return true;
    }
    default: {
        return RESTERR(req, HTTP_NOT_FOUND, "output format not found (available: json)");
    }
    }
}

static bool rest_mempool_info_basic(HTTPRequest* req, const std::string& strURIPart)
{
    return rest_mempool_info(req, strURIPart, false);
}

static bool rest_mempool_info_with_fee_histogram(HTTPRequest* req, const std::string& strURIPart)
{
    return rest_mempool_info(req, strURIPart, true);
}

static bool rest_mempool_contents(HTTPRequest* req, const std::string& strURIPart)
{
    if (!CheckWarmup(req)) return false;
    const CTxMemPool* mempool = GetMemPool(req);
    if (!mempool) return false;
    std::string param;
    const RetFormat rf = ParseDataFormat(param, strURIPart);

    switch (rf) {
    case RetFormat::JSON: {
        UniValue mempoolObject = MempoolToJSON(*mempool, true);

        std::string strJSON = mempoolObject.write() + "\n";
        req->WriteHeader("Content-Type", "application/json");
        req->WriteReply(HTTP_OK, strJSON);
        return true;
    }
    default: {
        return RESTERR(req, HTTP_NOT_FOUND, "output format not found (available: json)");
    }
    }
}

static bool rest_tx(HTTPRequest* req, const std::string& strURIPart)
{
    if (!CheckWarmup(req))
        return false;
    std::string hashStr;
    const RetFormat rf = ParseDataFormat(hashStr, strURIPart);

    uint256 hash;
    if (!ParseHashStr(hashStr, hash))
        return RESTERR(req, HTTP_BAD_REQUEST, "Invalid hash: " + hashStr);

    if (g_txindex) {
        g_txindex->BlockUntilSyncedToCurrentChain();
    }

    CTransactionRef tx;
    uint256 hashBlock = uint256();
    if (!GetTransaction(hash, tx, Params().GetConsensus(), hashBlock))
        return RESTERR(req, HTTP_NOT_FOUND, hashStr + " not found");

    switch (rf) {
    case RetFormat::BINARY: {
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION | RPCSerializationFlags());
        ssTx << tx;

        std::string binaryTx = ssTx.str();
        req->WriteHeader("Content-Type", "application/octet-stream");
        req->WriteReply(HTTP_OK, binaryTx);
        return true;
    }

    case RetFormat::HEX: {
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION | RPCSerializationFlags());
        ssTx << tx;

        std::string strHex = HexStr(ssTx.begin(), ssTx.end()) + "\n";
        req->WriteHeader("Content-Type", "text/plain");
        req->WriteReply(HTTP_OK, strHex);
        return true;
    }

    case RetFormat::JSON: {
        UniValue objTx(UniValue::VOBJ);
        TxToUniv(*tx, hashBlock, objTx);
        std::string strJSON = objTx.write() + "\n";
        req->WriteHeader("Content-Type", "application/json");
        req->WriteReply(HTTP_OK, strJSON);
        return true;
    }

    default: {
        return RESTERR(req, HTTP_NOT_FOUND, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }
}

static bool rest_getutxos(HTTPRequest* req, const std::string& strURIPart)
{
    if (!CheckWarmup(req))
        return false;
    std::string param;
    const RetFormat rf = ParseDataFormat(param, strURIPart);

    std::vector<std::string> uriParts;
    if (param.length() > 1)
    {
        std::string strUriParams = param.substr(1);
        boost::split(uriParts, strUriParams, boost::is_any_of("/"));
    }

    // throw exception in case of an empty request
    std::string strRequestMutable = req->ReadBody();
    if (strRequestMutable.length() == 0 && uriParts.size() == 0)
        return RESTERR(req, HTTP_BAD_REQUEST, "Error: empty request");

    bool fInputParsed = false;
    bool fCheckMemPool = false;
    std::vector<COutPoint> vOutPoints;

    // parse/deserialize input
    // input-format = output-format, rest/getutxos/bin requires binary input, gives binary output, ...

    if (uriParts.size() > 0)
    {
        //inputs is sent over URI scheme (/rest/getutxos/checkmempool/txid1-n/txid2-n/...)
        if (uriParts[0] == "checkmempool") fCheckMemPool = true;

        for (size_t i = (fCheckMemPool) ? 1 : 0; i < uriParts.size(); i++)
        {
            uint256 txid;
            int32_t nOutput;
            std::string strTxid = uriParts[i].substr(0, uriParts[i].find('-'));
            std::string strOutput = uriParts[i].substr(uriParts[i].find('-')+1);

            if (!ParseInt32(strOutput, &nOutput) || !IsHex(strTxid))
                return RESTERR(req, HTTP_BAD_REQUEST, "Parse error");

            txid.SetHex(strTxid);
            vOutPoints.push_back(COutPoint(txid, (uint32_t)nOutput));
        }

        if (vOutPoints.size() > 0)
            fInputParsed = true;
        else
            return RESTERR(req, HTTP_BAD_REQUEST, "Error: empty request");
    }

    switch (rf) {
    case RetFormat::HEX: {
        // convert hex to bin, continue then with bin part
        std::vector<unsigned char> strRequestV = ParseHex(strRequestMutable);
        strRequestMutable.assign(strRequestV.begin(), strRequestV.end());
    }

    case RetFormat::BINARY: {
        try {
            //deserialize only if user sent a request
            if (strRequestMutable.size() > 0)
            {
                if (fInputParsed) //don't allow sending input over URI and HTTP RAW DATA
                    return RESTERR(req, HTTP_BAD_REQUEST, "Combination of URI scheme inputs and raw post data is not allowed");

                CDataStream oss(SER_NETWORK, PROTOCOL_VERSION);
                oss << strRequestMutable;
                oss >> fCheckMemPool;
                oss >> vOutPoints;
            }
        } catch (const std::ios_base::failure&) {
            // abort in case of unreadable binary data
            return RESTERR(req, HTTP_BAD_REQUEST, "Parse error");
        }
        break;
    }

    case RetFormat::JSON: {
        if (!fInputParsed)
            return RESTERR(req, HTTP_BAD_REQUEST, "Error: empty request");
        break;
    }
    default: {
        return RESTERR(req, HTTP_NOT_FOUND, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }

    // limit max outpoints
    if (vOutPoints.size() > MAX_GETUTXOS_OUTPOINTS)
        return RESTERR(req, HTTP_BAD_REQUEST, strprintf("Error: max outpoints exceeded (max: %d, tried: %d)", MAX_GETUTXOS_OUTPOINTS, vOutPoints.size()));

    // check spentness and form a bitmap (as well as a JSON capable human-readable string representation)
    std::vector<unsigned char> bitmap;
    std::vector<CCoin> outs;
    std::string bitmapStringRepresentation;
    std::vector<bool> hits;
    bitmap.resize((vOutPoints.size() + 7) / 8);
    {
        auto process_utxos = [&vOutPoints, &outs, &hits](const CCoinsView& view, const CTxMemPool& mempool) {
            for (const COutPoint& vOutPoint : vOutPoints) {
                Coin coin;
                bool hit = !mempool.isSpent(vOutPoint) && view.GetCoin(vOutPoint, coin);
                hits.push_back(hit);
                if (hit) outs.emplace_back(std::move(coin));
            }
        };

        if (fCheckMemPool) {
            const CTxMemPool* mempool = GetMemPool(req);
            if (!mempool) return false;
            // use db+mempool as cache backend in case user likes to query mempool
            LOCK2(cs_main, mempool->cs);
            CCoinsViewCache& viewChain = ::ChainstateActive().CoinsTip();
            CCoinsViewMemPool viewMempool(&viewChain, *mempool);
            process_utxos(viewMempool, *mempool);
        } else {
            LOCK(cs_main);  // no need to lock mempool!
            process_utxos(::ChainstateActive().CoinsTip(), CTxMemPool());
        }

        for (size_t i = 0; i < hits.size(); ++i) {
            const bool hit = hits[i];
            bitmapStringRepresentation.append(hit ? "1" : "0"); // form a binary string representation (human-readable for json output)
            bitmap[i / 8] |= ((uint8_t)hit) << (i % 8);
        }
    }

    switch (rf) {
    case RetFormat::BINARY: {
        // serialize data
        // use exact same output as mentioned in Bip64
        CDataStream ssGetUTXOResponse(SER_NETWORK, PROTOCOL_VERSION);
        ssGetUTXOResponse << ::ChainActive().Height() << ::ChainActive().Tip()->GetBlockHash() << bitmap << outs;
        std::string ssGetUTXOResponseString = ssGetUTXOResponse.str();

        req->WriteHeader("Content-Type", "application/octet-stream");
        req->WriteReply(HTTP_OK, ssGetUTXOResponseString);
        return true;
    }

    case RetFormat::HEX: {
        CDataStream ssGetUTXOResponse(SER_NETWORK, PROTOCOL_VERSION);
        ssGetUTXOResponse << ::ChainActive().Height() << ::ChainActive().Tip()->GetBlockHash() << bitmap << outs;
        std::string strHex = HexStr(ssGetUTXOResponse.begin(), ssGetUTXOResponse.end()) + "\n";

        req->WriteHeader("Content-Type", "text/plain");
        req->WriteReply(HTTP_OK, strHex);
        return true;
    }

    case RetFormat::JSON: {
        UniValue objGetUTXOResponse(UniValue::VOBJ);

        // pack in some essentials
        // use more or less the same output as mentioned in Bip64
        objGetUTXOResponse.pushKV("chainHeight", ::ChainActive().Height());
        objGetUTXOResponse.pushKV("chaintipHash", ::ChainActive().Tip()->GetBlockHash().GetHex());
        objGetUTXOResponse.pushKV("bitmap", bitmapStringRepresentation);

        UniValue utxos(UniValue::VARR);
        for (const CCoin& coin : outs) {
            UniValue utxo(UniValue::VOBJ);
            utxo.pushKV("height", (int32_t)coin.nHeight);
            utxo.pushKV("value", ValueFromAmount(coin.out.nValue));

            // include the script in a json output
            UniValue o(UniValue::VOBJ);
            ScriptPubKeyToUniv(coin.out.scriptPubKey, o, true);
            utxo.pushKV("scriptPubKey", o);
            utxos.push_back(utxo);
        }
        objGetUTXOResponse.pushKV("utxos", utxos);

        // return json string
        std::string strJSON = objGetUTXOResponse.write() + "\n";
        req->WriteHeader("Content-Type", "application/json");
        req->WriteReply(HTTP_OK, strJSON);
        return true;
    }
    default: {
        return RESTERR(req, HTTP_NOT_FOUND, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }
}

static bool rest_blockhash_by_height(HTTPRequest* req,
                       const std::string& str_uri_part)
{
    if (!CheckWarmup(req)) return false;
    std::string height_str;
    const RetFormat rf = ParseDataFormat(height_str, str_uri_part);

    int32_t blockheight;
    if (!ParseInt32(height_str, &blockheight) || blockheight < 0) {
        return RESTERR(req, HTTP_BAD_REQUEST, "Invalid height: " + SanitizeString(height_str));
    }

    CBlockIndex* pblockindex = nullptr;
    {
        LOCK(cs_main);
        if (blockheight > ::ChainActive().Height()) {
            return RESTERR(req, HTTP_NOT_FOUND, "Block height out of range");
        }
        pblockindex = ::ChainActive()[blockheight];
    }
    switch (rf) {
    case RetFormat::BINARY: {
        CDataStream ss_blockhash(SER_NETWORK, PROTOCOL_VERSION);
        ss_blockhash << pblockindex->GetBlockHash();
        req->WriteHeader("Content-Type", "application/octet-stream");
        req->WriteReply(HTTP_OK, ss_blockhash.str());
        return true;
    }
    case RetFormat::HEX: {
        req->WriteHeader("Content-Type", "text/plain");
        req->WriteReply(HTTP_OK, pblockindex->GetBlockHash().GetHex() + "\n");
        return true;
    }
    case RetFormat::JSON: {
        req->WriteHeader("Content-Type", "application/json");
        UniValue resp = UniValue(UniValue::VOBJ);
        resp.pushKV("blockhash", pblockindex->GetBlockHash().GetHex());
        req->WriteReply(HTTP_OK, resp.write() + "\n");
        return true;
    }
    default: {
        return RESTERR(req, HTTP_NOT_FOUND, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }
}

static bool rest_getfee(HTTPRequest* req, const std::string& strURIPart) {
    if (!CheckWarmup(req)) {
        return false;
    }
    std::string param;
    const RetFormat rf = ParseDataFormat(param, strURIPart);
    switch (rf) {
    case RetFormat::JSON: {
        std::vector<std::string> path;
        boost::split(path, param, boost::is_any_of("/"));
        path.erase(path.begin());
        // check url scheme is correct
        if (path.size() != 2) {
            return RESTERR(req, HTTP_BAD_REQUEST, "Path must be /rest/fee/<MODE>/<TARGET>.json");
        }
        // check estimation mode is valid
        const auto modestr = ToUpper(path[0]);
        FeeEstimateMode mode;
        if (!FeeModeFromString(modestr, mode)){
            return RESTERR(req, HTTP_BAD_REQUEST, "<MODE> must be one of <unset|economical|conservative>");
        };

        // type conversions for estimateSmartFee
        bool conservative = mode != FeeEstimateMode::ECONOMICAL;
        int64_t conf_target;
        try {
             conf_target = atoi64(path[1]);
        } catch (std::invalid_argument&) {
            return RESTERR(req, HTTP_BAD_REQUEST, strprintf("Unable to parse confirmation target to int"));
        };
        unsigned int max_target = ::feeEstimator.HighestTargetTracked(FeeEstimateHorizon::LONG_HALFLIFE);
        if (conf_target < 1 || (unsigned int)conf_target > max_target) {
            return RESTERR(req, HTTP_BAD_REQUEST, strprintf("Invalid confirmation target, must be in between %u - %u", 1, max_target));
        };

        // perform fee estimation
        FeeCalculation feeCalc;
        CFeeRate estimatedfee = ::feeEstimator.estimateSmartFee(conf_target, &feeCalc, conservative);

        // create json for replying
        UniValue feejson(UniValue::VOBJ);
        if (estimatedfee != CFeeRate(0)) {
            feejson.pushKV("feerate", ValueFromAmount(estimatedfee.GetFeePerK()));
        } else {
            return RESTERR(req, HTTP_SERVICE_UNAVAILABLE, "Insufficient data or no feerate found");
        }
        feejson.pushKV("blocks", feeCalc.returnedTarget);

        // reply
        std::string strJSON = feejson.write() + "\n";
        req->WriteHeader("Content-Type", "application/json");
        req->WriteReply(HTTP_OK, strJSON);
        return true;
    }
    default: {
        return RESTERR(req, HTTP_NOT_FOUND, "output format not found (available: json)");
    }
    }
}

static const struct {
    const char* prefix;
    bool (*handler)(HTTPRequest* req, const std::string& strReq);
} uri_prefixes[] = {
      {"/rest/tx/", rest_tx},
      {"/rest/block/notxdetails/", rest_block_notxdetails},
      {"/rest/block/", rest_block_extended},
      {"/rest/blockfilter/", rest_block_filter},
      {"/rest/blockfilterheaders/", rest_filter_header},
      {"/rest/chaininfo", rest_chaininfo},
      {"/rest/mempool/info", rest_mempool_info_basic},
      {"/rest/mempool/info/with_fee_histogram", rest_mempool_info_with_fee_histogram},
      {"/rest/mempool/contents", rest_mempool_contents},
      {"/rest/headers/", rest_headers},
      {"/rest/getutxos", rest_getutxos},
      {"/rest/blockhashbyheight/", rest_blockhash_by_height},
      {"/rest/fee", rest_getfee},
};

void StartREST()
{
    for (unsigned int i = 0; i < ARRAYLEN(uri_prefixes); i++)
        RegisterHTTPHandler(uri_prefixes[i].prefix, false, uri_prefixes[i].handler);
}

void InterruptREST()
{
}

void StopREST()
{
    for (unsigned int i = 0; i < ARRAYLEN(uri_prefixes); i++)
        UnregisterHTTPHandler(uri_prefixes[i].prefix, false);
}
