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

#ifndef TDENGINE_TPAGEDFILE_H
#define TDENGINE_TPAGEDFILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "tlist.h"
#include "thash.h"
#include "os.h"
#include "tlockfree.h"

typedef struct SArray* SIDList;
typedef struct SPageInfo SPageInfo;
typedef struct SDiskbasedBuf SDiskbasedBuf;

#define DEFAULT_INTERN_BUF_PAGE_SIZE  (1024L)                          // in bytes
#define DEFAULT_PAGE_SIZE             (16384L)

typedef struct SFilePage {
  int64_t num;
  char    data[];
} SFilePage;

/**
 * create disk-based result buffer
 * @param pResultBuf
 * @param rowSize
 * @param pagesize
 * @param inMemPages
 * @param handle
 * @return
 */
int32_t createDiskbasedBuffer(SDiskbasedBuf** pResultBuf, int32_t pagesize, int32_t inMemBufSize, uint64_t qId, const char* dir);

/**
 *
 * @param pResultBuf
 * @param groupId
 * @param pageId
 * @return
 */
SFilePage* getNewDataBuf(SDiskbasedBuf* pResultBuf, int32_t groupId, int32_t* pageId);

/**
 *
 * @param pResultBuf
 * @param groupId
 * @return
 */
SIDList getDataBufPagesIdList(SDiskbasedBuf* pResultBuf, int32_t groupId);

/**
 * get the specified buffer page by id
 * @param pResultBuf
 * @param id
 * @return
 */
SFilePage* getResBufPage(SDiskbasedBuf* pResultBuf, int32_t id);

/**
 * release the referenced buf pages
 * @param pResultBuf
 * @param page
 */
void releaseResBufPage(SDiskbasedBuf* pResultBuf, void* page);

/**
 *
 * @param pResultBuf
 * @param pi
 */
void releaseResBufPageInfo(SDiskbasedBuf* pResultBuf, struct SPageInfo* pi);

/**
 * get the total buffer size in the format of disk file
 * @param pResultBuf
 * @return
 */
size_t getResBufSize(const SDiskbasedBuf* pResultBuf);

/**
 * get the number of groups in the result buffer
 * @param pResultBuf
 * @return
 */
size_t getNumOfResultBufGroupId(const SDiskbasedBuf* pResultBuf);

/**
 * destroy result buffer
 * @param pResultBuf
 */
void destroyResultBuf(SDiskbasedBuf* pResultBuf);

/**
 *
 * @param pList
 * @return
 */
SPageInfo* getLastPageInfo(SIDList pList);

/**
 *
 * @param pPgInfo
 * @return
 */
int32_t getPageId(const SPageInfo* pPgInfo);

/**
 * Return the buffer page size.
 * @param pResultBuf
 * @return
 */
int32_t getBufPageSize(const SDiskbasedBuf* pResultBuf);

/**
 *
 * @param pResultBuf
 * @return
 */
bool isAllDataInMemBuf(const SDiskbasedBuf* pResultBuf);

#ifdef __cplusplus
}
#endif

#endif  // TDENGINE_TPAGEDFILE_H
