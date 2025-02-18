/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "filter.h"
#include "function.h"
#include "functionMgt.h"
#include "os.h"
#include "querynodes.h"
#include "tfill.h"
#include "tname.h"
#include "tref.h"

#include "tdatablock.h"
#include "tglobal.h"
#include "tmsg.h"
#include "tsort.h"
#include "ttime.h"

#include "executorimpl.h"
#include "index.h"
#include "query.h"
#include "tcompare.h"
#include "thash.h"
#include "ttypes.h"
#include "vnode.h"

typedef struct SFetchRspHandleWrapper {
  uint32_t exchangeId;
  int32_t  sourceIndex;
} SFetchRspHandleWrapper;

static void destroyExchangeOperatorInfo(void* param);
static void freeBlock(void* pParam);
static void freeSourceDataInfo(void* param);
static void* setAllSourcesCompleted(SOperatorInfo* pOperator, int64_t startTs);

static int32_t loadRemoteDataCallback(void* param, SDataBuf* pMsg, int32_t code);
static int32_t doSendFetchDataRequest(SExchangeInfo* pExchangeInfo, SExecTaskInfo* pTaskInfo, int32_t sourceIndex);
static int32_t getCompletedSources(const SArray* pArray);
static int32_t prepareConcurrentlyLoad(SOperatorInfo* pOperator);
static int32_t seqLoadRemoteData(SOperatorInfo* pOperator);
static int32_t prepareLoadRemoteData(SOperatorInfo* pOperator);

static void concurrentlyLoadRemoteDataImpl(SOperatorInfo* pOperator, SExchangeInfo* pExchangeInfo,
                                           SExecTaskInfo* pTaskInfo) {
  int32_t code = 0;
  size_t  totalSources = taosArrayGetSize(pExchangeInfo->pSourceDataInfo);
  int32_t completed = getCompletedSources(pExchangeInfo->pSourceDataInfo);
  if (completed == totalSources) {
    setAllSourcesCompleted(pOperator, pExchangeInfo->openedTs);
    return;
  }

  while (1) {
    tsem_wait(&pExchangeInfo->ready);
    if (isTaskKilled(pTaskInfo)) {
      longjmp(pTaskInfo->env, TSDB_CODE_TSC_QUERY_CANCELLED);
    }

    for (int32_t i = 0; i < totalSources; ++i) {
      SSourceDataInfo* pDataInfo = taosArrayGet(pExchangeInfo->pSourceDataInfo, i);
      if (pDataInfo->status == EX_SOURCE_DATA_EXHAUSTED) {
        continue;
      }

      if (pDataInfo->status != EX_SOURCE_DATA_READY) {
        continue;
      }

      if (pDataInfo->code != TSDB_CODE_SUCCESS) {
        code = pDataInfo->code;
        goto _error;
      }

      SRetrieveTableRsp*     pRsp = pDataInfo->pRsp;
      SDownstreamSourceNode* pSource = taosArrayGet(pExchangeInfo->pSources, i);

      // todo
      SLoadRemoteDataInfo* pLoadInfo = &pExchangeInfo->loadInfo;
      if (pRsp->numOfRows == 0) {
        pDataInfo->status = EX_SOURCE_DATA_EXHAUSTED;
        qDebug("%s vgId:%d, taskId:0x%" PRIx64 " execId:%d index:%d completed, rowsOfSource:%" PRIu64
                   ", totalRows:%" PRIu64 ", try next %d/%" PRIzu,
               GET_TASKID(pTaskInfo), pSource->addr.nodeId, pSource->taskId, pSource->execId, i, pDataInfo->totalRows,
               pExchangeInfo->loadInfo.totalRows, i + 1, totalSources);
        taosMemoryFreeClear(pDataInfo->pRsp);
        break;
      }

      SRetrieveTableRsp* pRetrieveRsp = pDataInfo->pRsp;
      int32_t            index = 0;
      char*              pStart = pRetrieveRsp->data;
      while (index++ < pRetrieveRsp->numOfBlocks) {
        SSDataBlock* pb = createOneDataBlock(pExchangeInfo->pDummyBlock, false);
        code = extractDataBlockFromFetchRsp(pb, pStart, NULL, &pStart);
        if (code != 0) {
          taosMemoryFreeClear(pDataInfo->pRsp);
          goto _error;
        }

        taosArrayPush(pExchangeInfo->pResultBlockList, &pb);
      }

      updateLoadRemoteInfo(pLoadInfo, pRetrieveRsp->numOfRows, pRetrieveRsp->compLen, pExchangeInfo->openedTs, pOperator);

      if (pRsp->completed == 1) {
        pDataInfo->status = EX_SOURCE_DATA_EXHAUSTED;
        qDebug("%s fetch msg rsp from vgId:%d, taskId:0x%" PRIx64
                   " execId:%d index:%d completed, blocks:%d, numOfRows:%d, rowsOfSource:%" PRIu64 ", totalRows:%" PRIu64
                   ", total:%.2f Kb, try next %d/%" PRIzu,
               GET_TASKID(pTaskInfo), pSource->addr.nodeId, pSource->taskId, pSource->execId, i, pRsp->numOfBlocks,
               pRsp->numOfRows, pDataInfo->totalRows, pLoadInfo->totalRows, pLoadInfo->totalSize / 1024.0,
               i + 1, totalSources);
      } else {
        qDebug("%s fetch msg rsp from vgId:%d, taskId:0x%" PRIx64
                   " execId:%d blocks:%d, numOfRows:%d, totalRows:%" PRIu64 ", total:%.2f Kb",
               GET_TASKID(pTaskInfo), pSource->addr.nodeId, pSource->taskId, pSource->execId, pRsp->numOfBlocks,
               pRsp->numOfRows, pLoadInfo->totalRows, pLoadInfo->totalSize / 1024.0);
      }

      taosMemoryFreeClear(pDataInfo->pRsp);

      if (pDataInfo->status != EX_SOURCE_DATA_EXHAUSTED) {
        pDataInfo->status = EX_SOURCE_DATA_NOT_READY;
        code = doSendFetchDataRequest(pExchangeInfo, pTaskInfo, i);
        if (code != TSDB_CODE_SUCCESS) {
          taosMemoryFreeClear(pDataInfo->pRsp);
          goto _error;
        }
      }
      return;
    }  // end loop

    int32_t complete1 = getCompletedSources(pExchangeInfo->pSourceDataInfo);
    if (complete1 == totalSources) {
      qDebug("all sources are completed, %s", GET_TASKID(pTaskInfo));
      return;
    }
  }

  _error:
  pTaskInfo->code = code;
}

static SSDataBlock* doLoadRemoteDataImpl(SOperatorInfo* pOperator) {
  SExchangeInfo* pExchangeInfo = pOperator->info;
  SExecTaskInfo* pTaskInfo = pOperator->pTaskInfo;

  pTaskInfo->code = pOperator->fpSet._openFn(pOperator);
  if (pTaskInfo->code != TSDB_CODE_SUCCESS) {
    return NULL;
  }

  size_t totalSources = taosArrayGetSize(pExchangeInfo->pSources);

  SLoadRemoteDataInfo* pLoadInfo = &pExchangeInfo->loadInfo;
  if (pOperator->status == OP_EXEC_DONE) {
    qDebug("%s all %" PRIzu " source(s) are exhausted, total rows:%" PRIu64 " bytes:%" PRIu64 ", elapsed:%.2f ms",
           GET_TASKID(pTaskInfo), totalSources, pLoadInfo->totalRows, pLoadInfo->totalSize,
           pLoadInfo->totalElapsed / 1000.0);
    return NULL;
  }

  size_t size = taosArrayGetSize(pExchangeInfo->pResultBlockList);
  if (size == 0 || pExchangeInfo->rspBlockIndex >= size) {
    pExchangeInfo->rspBlockIndex = 0;
    taosArrayClearEx(pExchangeInfo->pResultBlockList, freeBlock);
    if (pExchangeInfo->seqLoadData) {
      seqLoadRemoteData(pOperator);
    } else {
      concurrentlyLoadRemoteDataImpl(pOperator, pExchangeInfo, pTaskInfo);
    }

    if (taosArrayGetSize(pExchangeInfo->pResultBlockList) == 0) {
      return NULL;
    }
  }

  // we have buffered retrieved datablock, return it directly
  return taosArrayGetP(pExchangeInfo->pResultBlockList, pExchangeInfo->rspBlockIndex++);
}

static SSDataBlock* doLoadRemoteData(SOperatorInfo* pOperator) {
  SExchangeInfo* pExchangeInfo = pOperator->info;
  SExecTaskInfo* pTaskInfo = pOperator->pTaskInfo;

  if (pOperator->status == OP_EXEC_DONE) {
    return NULL;
  }

  while (1) {
    SSDataBlock* pBlock = doLoadRemoteDataImpl(pOperator);
    if (pBlock == NULL) {
      return NULL;
    }

    SLimitInfo* pLimitInfo = &pExchangeInfo->limitInfo;
    if (hasLimitOffsetInfo(pLimitInfo)) {
      int32_t status = handleLimitOffset(pOperator, pLimitInfo, pBlock, false);
      if (status == PROJECT_RETRIEVE_CONTINUE) {
        continue;
      } else if (status == PROJECT_RETRIEVE_DONE) {
        size_t rows = pBlock->info.rows;
        pExchangeInfo->limitInfo.numOfOutputRows += rows;

        if (rows == 0) {
          setOperatorCompleted(pOperator);
          return NULL;
        } else {
          return pBlock;
        }
      }
    } else {
      return pBlock;
    }
  }
}

static int32_t initDataSource(int32_t numOfSources, SExchangeInfo* pInfo, const char* id) {
  pInfo->pSourceDataInfo = taosArrayInit(numOfSources, sizeof(SSourceDataInfo));
  if (pInfo->pSourceDataInfo == NULL) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  for (int32_t i = 0; i < numOfSources; ++i) {
    SSourceDataInfo dataInfo = {0};
    dataInfo.status = EX_SOURCE_DATA_NOT_READY;
    dataInfo.taskId = id;
    dataInfo.index = i;
    SSourceDataInfo* pDs = taosArrayPush(pInfo->pSourceDataInfo, &dataInfo);
    if (pDs == NULL) {
      taosArrayDestroy(pInfo->pSourceDataInfo);
      return TSDB_CODE_OUT_OF_MEMORY;
    }
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t initExchangeOperator(SExchangePhysiNode* pExNode, SExchangeInfo* pInfo, const char* id) {
  size_t numOfSources = LIST_LENGTH(pExNode->pSrcEndPoints);

  if (numOfSources == 0) {
    qError("%s invalid number: %d of sources in exchange operator", id, (int32_t)numOfSources);
    return TSDB_CODE_INVALID_PARA;
  }

  pInfo->pSources = taosArrayInit(numOfSources, sizeof(SDownstreamSourceNode));
  if (pInfo->pSources == NULL) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  for (int32_t i = 0; i < numOfSources; ++i) {
    SDownstreamSourceNode* pNode = (SDownstreamSourceNode*)nodesListGetNode((SNodeList*)pExNode->pSrcEndPoints, i);
    taosArrayPush(pInfo->pSources, pNode);
  }

  initLimitInfo(pExNode->node.pLimit, pExNode->node.pSlimit, &pInfo->limitInfo);
  pInfo->self = taosAddRef(exchangeObjRefPool, pInfo);

  return initDataSource(numOfSources, pInfo, id);
}

SOperatorInfo* createExchangeOperatorInfo(void* pTransporter, SExchangePhysiNode* pExNode, SExecTaskInfo* pTaskInfo) {
  SExchangeInfo* pInfo = taosMemoryCalloc(1, sizeof(SExchangeInfo));
  SOperatorInfo* pOperator = taosMemoryCalloc(1, sizeof(SOperatorInfo));
  if (pInfo == NULL || pOperator == NULL) {
    goto _error;
  }

  int32_t code = initExchangeOperator(pExNode, pInfo, GET_TASKID(pTaskInfo));
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }

  tsem_init(&pInfo->ready, 0, 0);
  pInfo->pDummyBlock = createResDataBlock(pExNode->node.pOutputDataBlockDesc);
  pInfo->pResultBlockList = taosArrayInit(1, POINTER_BYTES);

  SExchangeOpStopInfo stopInfo = {QUERY_NODE_PHYSICAL_PLAN_EXCHANGE, pInfo->self};
  qAppendTaskStopInfo(pTaskInfo, &stopInfo);
  
  pInfo->seqLoadData = false;
  pInfo->pTransporter = pTransporter;

  setOperatorInfo(pOperator, "ExchangeOperator", QUERY_NODE_PHYSICAL_PLAN_EXCHANGE, false, OP_NOT_OPENED, pInfo, pTaskInfo);
  pOperator->exprSupp.numOfExprs = taosArrayGetSize(pInfo->pDummyBlock->pDataBlock);

  pOperator->fpSet = createOperatorFpSet(prepareLoadRemoteData, doLoadRemoteData, NULL, destroyExchangeOperatorInfo, NULL);
  return pOperator;

  _error:
  if (pInfo != NULL) {
    doDestroyExchangeOperatorInfo(pInfo);
  }

  taosMemoryFreeClear(pOperator);
  pTaskInfo->code = code;
  return NULL;
}

void destroyExchangeOperatorInfo(void* param) {
  SExchangeInfo* pExInfo = (SExchangeInfo*)param;
  taosRemoveRef(exchangeObjRefPool, pExInfo->self);
}

void freeBlock(void* pParam) {
  SSDataBlock* pBlock = *(SSDataBlock**)pParam;
  blockDataDestroy(pBlock);
}

void freeSourceDataInfo(void* p) {
  SSourceDataInfo* pInfo = (SSourceDataInfo*)p;
  taosMemoryFreeClear(pInfo->pRsp);
}

void doDestroyExchangeOperatorInfo(void* param) {
  SExchangeInfo* pExInfo = (SExchangeInfo*)param;

  taosArrayDestroy(pExInfo->pSources);
  taosArrayDestroyEx(pExInfo->pSourceDataInfo, freeSourceDataInfo);

  if (pExInfo->pResultBlockList != NULL) {
    taosArrayDestroyEx(pExInfo->pResultBlockList, freeBlock);
    pExInfo->pResultBlockList = NULL;
  }

  blockDataDestroy(pExInfo->pDummyBlock);

  tsem_destroy(&pExInfo->ready);
  taosMemoryFreeClear(param);
}

int32_t loadRemoteDataCallback(void* param, SDataBuf* pMsg, int32_t code) {
  SFetchRspHandleWrapper* pWrapper = (SFetchRspHandleWrapper*)param;

  SExchangeInfo* pExchangeInfo = taosAcquireRef(exchangeObjRefPool, pWrapper->exchangeId);
  if (pExchangeInfo == NULL) {
    qWarn("failed to acquire exchange operator, since it may have been released");
    taosMemoryFree(pMsg->pData);
    return TSDB_CODE_SUCCESS;
  }

  int32_t          index = pWrapper->sourceIndex;
  SSourceDataInfo* pSourceDataInfo = taosArrayGet(pExchangeInfo->pSourceDataInfo, index);

  if (code == TSDB_CODE_SUCCESS) {
    pSourceDataInfo->pRsp = pMsg->pData;

    SRetrieveTableRsp* pRsp = pSourceDataInfo->pRsp;
    pRsp->numOfRows = htonl(pRsp->numOfRows);
    pRsp->compLen = htonl(pRsp->compLen);
    pRsp->numOfCols = htonl(pRsp->numOfCols);
    pRsp->useconds = htobe64(pRsp->useconds);
    pRsp->numOfBlocks = htonl(pRsp->numOfBlocks);

    ASSERT(pRsp != NULL);
    qDebug("%s fetch rsp received, index:%d, blocks:%d, rows:%d", pSourceDataInfo->taskId, index, pRsp->numOfBlocks,
           pRsp->numOfRows);
  } else {
    taosMemoryFree(pMsg->pData);
    pSourceDataInfo->code = code;
    qDebug("%s fetch rsp received, index:%d, error:%s", pSourceDataInfo->taskId, index, tstrerror(code));
  }

  pSourceDataInfo->status = EX_SOURCE_DATA_READY;

  tsem_post(&pExchangeInfo->ready);
  taosReleaseRef(exchangeObjRefPool, pWrapper->exchangeId);

  return TSDB_CODE_SUCCESS;
}

int32_t doSendFetchDataRequest(SExchangeInfo* pExchangeInfo, SExecTaskInfo* pTaskInfo, int32_t sourceIndex) {
  size_t totalSources = taosArrayGetSize(pExchangeInfo->pSources);

  SDownstreamSourceNode* pSource = taosArrayGet(pExchangeInfo->pSources, sourceIndex);
  SSourceDataInfo*       pDataInfo = taosArrayGet(pExchangeInfo->pSourceDataInfo, sourceIndex);

  ASSERT(pDataInfo->status == EX_SOURCE_DATA_NOT_READY);

  SFetchRspHandleWrapper* pWrapper = taosMemoryCalloc(1, sizeof(SFetchRspHandleWrapper));
  pWrapper->exchangeId = pExchangeInfo->self;
  pWrapper->sourceIndex = sourceIndex;

  if (pSource->localExec) {
    SDataBuf pBuf = {0};
    int32_t  code =
        (*pTaskInfo->localFetch.fp)(pTaskInfo->localFetch.handle, pSource->schedId, pTaskInfo->id.queryId,
                                    pSource->taskId, 0, pSource->execId, &pBuf.pData, pTaskInfo->localFetch.explainRes);
    loadRemoteDataCallback(pWrapper, &pBuf, code);
    taosMemoryFree(pWrapper);
  } else {
    SResFetchReq* pMsg = taosMemoryCalloc(1, sizeof(SResFetchReq));
    if (NULL == pMsg) {
      pTaskInfo->code = TSDB_CODE_QRY_OUT_OF_MEMORY;
      taosMemoryFree(pWrapper);
      return pTaskInfo->code;
    }

    qDebug("%s build fetch msg and send to vgId:%d, ep:%s, taskId:0x%" PRIx64 ", execId:%d, %d/%" PRIzu,
           GET_TASKID(pTaskInfo), pSource->addr.nodeId, pSource->addr.epSet.eps[0].fqdn, pSource->taskId,
           pSource->execId, sourceIndex, totalSources);

    pMsg->header.vgId = htonl(pSource->addr.nodeId);
    pMsg->sId = htobe64(pSource->schedId);
    pMsg->taskId = htobe64(pSource->taskId);
    pMsg->queryId = htobe64(pTaskInfo->id.queryId);
    pMsg->execId = htonl(pSource->execId);

    // send the fetch remote task result reques
    SMsgSendInfo* pMsgSendInfo = taosMemoryCalloc(1, sizeof(SMsgSendInfo));
    if (NULL == pMsgSendInfo) {
      taosMemoryFreeClear(pMsg);
      taosMemoryFree(pWrapper);
      qError("%s prepare message %d failed", GET_TASKID(pTaskInfo), (int32_t)sizeof(SMsgSendInfo));
      pTaskInfo->code = TSDB_CODE_QRY_OUT_OF_MEMORY;
      return pTaskInfo->code;
    }

    pMsgSendInfo->param = pWrapper;
    pMsgSendInfo->paramFreeFp = taosMemoryFree;
    pMsgSendInfo->msgInfo.pData = pMsg;
    pMsgSendInfo->msgInfo.len = sizeof(SResFetchReq);
    pMsgSendInfo->msgType = pSource->fetchMsgType;
    pMsgSendInfo->fp = loadRemoteDataCallback;

    int64_t transporterId = 0;
    int32_t code =
        asyncSendMsgToServer(pExchangeInfo->pTransporter, &pSource->addr.epSet, &transporterId, pMsgSendInfo);
  }

  return TSDB_CODE_SUCCESS;
}

void updateLoadRemoteInfo(SLoadRemoteDataInfo* pInfo, int32_t numOfRows, int32_t dataLen, int64_t startTs,
                          SOperatorInfo* pOperator) {
  pInfo->totalRows += numOfRows;
  pInfo->totalSize += dataLen;
  pInfo->totalElapsed += (taosGetTimestampUs() - startTs);
  pOperator->resultInfo.totalRows += numOfRows;
}

int32_t extractDataBlockFromFetchRsp(SSDataBlock* pRes, char* pData, SArray* pColList, char** pNextStart) {
  if (pColList == NULL) {  // data from other sources
    blockDataCleanup(pRes);
    *pNextStart = (char*)blockDecode(pRes, pData);
  } else {  // extract data according to pColList
    char* pStart = pData;

    int32_t numOfCols = htonl(*(int32_t*)pStart);
    pStart += sizeof(int32_t);

    // todo refactor:extract method
    SSysTableSchema* pSchema = (SSysTableSchema*)pStart;
    for (int32_t i = 0; i < numOfCols; ++i) {
      SSysTableSchema* p = (SSysTableSchema*)pStart;

      p->colId = htons(p->colId);
      p->bytes = htonl(p->bytes);
      pStart += sizeof(SSysTableSchema);
    }

    SSDataBlock* pBlock = createDataBlock();
    for (int32_t i = 0; i < numOfCols; ++i) {
      SColumnInfoData idata = createColumnInfoData(pSchema[i].type, pSchema[i].bytes, pSchema[i].colId);
      blockDataAppendColInfo(pBlock, &idata);
    }

    blockDecode(pBlock, pStart);
    blockDataEnsureCapacity(pRes, pBlock->info.rows);

    // data from mnode
    pRes->info.rows = pBlock->info.rows;
    relocateColumnData(pRes, pColList, pBlock->pDataBlock, false);
    blockDataDestroy(pBlock);
  }

  // todo move this to time window aggregator, since the primary timestamp may not be known by exchange operator.
  blockDataUpdateTsWindow(pRes, 0);
  return TSDB_CODE_SUCCESS;
}

void* setAllSourcesCompleted(SOperatorInfo* pOperator, int64_t startTs) {
  SExchangeInfo* pExchangeInfo = pOperator->info;
  SExecTaskInfo* pTaskInfo = pOperator->pTaskInfo;

  int64_t              el = taosGetTimestampUs() - startTs;
  SLoadRemoteDataInfo* pLoadInfo = &pExchangeInfo->loadInfo;

  pLoadInfo->totalElapsed += el;

  size_t totalSources = taosArrayGetSize(pExchangeInfo->pSources);
  qDebug("%s all %" PRIzu " sources are exhausted, total rows: %" PRIu64 " bytes:%" PRIu64 ", elapsed:%.2f ms",
         GET_TASKID(pTaskInfo), totalSources, pLoadInfo->totalRows, pLoadInfo->totalSize,
         pLoadInfo->totalElapsed / 1000.0);

  setOperatorCompleted(pOperator);
  return NULL;
}

int32_t getCompletedSources(const SArray* pArray) {
  size_t total = taosArrayGetSize(pArray);

  int32_t completed = 0;
  for (int32_t k = 0; k < total; ++k) {
    SSourceDataInfo* p = taosArrayGet(pArray, k);
    if (p->status == EX_SOURCE_DATA_EXHAUSTED) {
      completed += 1;
    }
  }

  return completed;
}

int32_t prepareConcurrentlyLoad(SOperatorInfo* pOperator) {
  SExchangeInfo* pExchangeInfo = pOperator->info;
  SExecTaskInfo* pTaskInfo = pOperator->pTaskInfo;

  size_t  totalSources = taosArrayGetSize(pExchangeInfo->pSources);
  int64_t startTs = taosGetTimestampUs();

  // Asynchronously send all fetch requests to all sources.
  for (int32_t i = 0; i < totalSources; ++i) {
    int32_t code = doSendFetchDataRequest(pExchangeInfo, pTaskInfo, i);
    if (code != TSDB_CODE_SUCCESS) {
      pTaskInfo->code = code;
      return code;
    }
  }

  int64_t endTs = taosGetTimestampUs();
  qDebug("%s send all fetch requests to %" PRIzu " sources completed, elapsed:%.2fms", GET_TASKID(pTaskInfo),
         totalSources, (endTs - startTs) / 1000.0);

  pOperator->status = OP_RES_TO_RETURN;
  pOperator->cost.openCost = taosGetTimestampUs() - startTs;

  tsem_wait(&pExchangeInfo->ready);
  if (isTaskKilled(pTaskInfo)) {
    longjmp(pTaskInfo->env, TSDB_CODE_TSC_QUERY_CANCELLED);
  }
  
  tsem_post(&pExchangeInfo->ready);
  return TSDB_CODE_SUCCESS;
}

int32_t seqLoadRemoteData(SOperatorInfo* pOperator) {
  SExchangeInfo* pExchangeInfo = pOperator->info;
  SExecTaskInfo* pTaskInfo = pOperator->pTaskInfo;

  size_t  totalSources = taosArrayGetSize(pExchangeInfo->pSources);
  int64_t startTs = taosGetTimestampUs();

  while (1) {
    if (pExchangeInfo->current >= totalSources) {
      setAllSourcesCompleted(pOperator, startTs);
      return TSDB_CODE_SUCCESS;
    }

    doSendFetchDataRequest(pExchangeInfo, pTaskInfo, pExchangeInfo->current);
    tsem_wait(&pExchangeInfo->ready);
    if (isTaskKilled(pTaskInfo)) {
      longjmp(pTaskInfo->env, TSDB_CODE_TSC_QUERY_CANCELLED);
    }

    SSourceDataInfo*       pDataInfo = taosArrayGet(pExchangeInfo->pSourceDataInfo, pExchangeInfo->current);
    SDownstreamSourceNode* pSource = taosArrayGet(pExchangeInfo->pSources, pExchangeInfo->current);

    if (pDataInfo->code != TSDB_CODE_SUCCESS) {
      qError("%s vgId:%d, taskID:0x%" PRIx64 " execId:%d error happens, code:%s", GET_TASKID(pTaskInfo),
             pSource->addr.nodeId, pSource->taskId, pSource->execId, tstrerror(pDataInfo->code));
      pOperator->pTaskInfo->code = pDataInfo->code;
      return pOperator->pTaskInfo->code;
    }

    SRetrieveTableRsp*   pRsp = pDataInfo->pRsp;
    SLoadRemoteDataInfo* pLoadInfo = &pExchangeInfo->loadInfo;
    if (pRsp->numOfRows == 0) {
      qDebug("%s vgId:%d, taskID:0x%" PRIx64 " execId:%d %d of total completed, rowsOfSource:%" PRIu64
                 ", totalRows:%" PRIu64 " try next",
             GET_TASKID(pTaskInfo), pSource->addr.nodeId, pSource->taskId, pSource->execId, pExchangeInfo->current + 1,
             pDataInfo->totalRows, pLoadInfo->totalRows);

      pDataInfo->status = EX_SOURCE_DATA_EXHAUSTED;
      pExchangeInfo->current += 1;
      taosMemoryFreeClear(pDataInfo->pRsp);
      continue;
    }

    SRetrieveTableRsp* pRetrieveRsp = pDataInfo->pRsp;

    char*   pStart = pRetrieveRsp->data;
    int32_t code = extractDataBlockFromFetchRsp(NULL, pStart, NULL, &pStart);

    if (pRsp->completed == 1) {
      qDebug("%s fetch msg rsp from vgId:%d, taskId:0x%" PRIx64 " execId:%d numOfRows:%d, rowsOfSource:%" PRIu64
                 ", totalRows:%" PRIu64 ", totalBytes:%" PRIu64 " try next %d/%" PRIzu,
             GET_TASKID(pTaskInfo), pSource->addr.nodeId, pSource->taskId, pSource->execId, pRetrieveRsp->numOfRows,
             pDataInfo->totalRows, pLoadInfo->totalRows, pLoadInfo->totalSize, pExchangeInfo->current + 1,
             totalSources);

      pDataInfo->status = EX_SOURCE_DATA_EXHAUSTED;
      pExchangeInfo->current += 1;
    } else {
      qDebug("%s fetch msg rsp from vgId:%d, taskId:0x%" PRIx64 " execId:%d numOfRows:%d, totalRows:%" PRIu64
                 ", totalBytes:%" PRIu64,
             GET_TASKID(pTaskInfo), pSource->addr.nodeId, pSource->taskId, pSource->execId, pRetrieveRsp->numOfRows,
             pLoadInfo->totalRows, pLoadInfo->totalSize);
    }

    updateLoadRemoteInfo(pLoadInfo, pRetrieveRsp->numOfRows, pRetrieveRsp->compLen, startTs, pOperator);
    pDataInfo->totalRows += pRetrieveRsp->numOfRows;

    taosMemoryFreeClear(pDataInfo->pRsp);
    return TSDB_CODE_SUCCESS;
  }
}

int32_t prepareLoadRemoteData(SOperatorInfo* pOperator) {
  if (OPTR_IS_OPENED(pOperator)) {
    return TSDB_CODE_SUCCESS;
  }

  int64_t st = taosGetTimestampUs();

  SExchangeInfo* pExchangeInfo = pOperator->info;
  if (!pExchangeInfo->seqLoadData) {
    int32_t code = prepareConcurrentlyLoad(pOperator);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }
    pExchangeInfo->openedTs = taosGetTimestampUs();
  }

  OPTR_SET_OPENED(pOperator);
  pOperator->cost.openCost = (taosGetTimestampUs() - st) / 1000.0;
  return TSDB_CODE_SUCCESS;
}
