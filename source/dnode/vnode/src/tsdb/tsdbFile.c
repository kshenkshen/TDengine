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

#include "tsdb.h"

void tsdbDataFileName(STsdb *pTsdb, SDFileSet *pDFileSet, int8_t ftype, char fname[]) {
  STfs *pTfs = pTsdb->pVnode->pTfs;

  switch (ftype) {
    case TSDB_HEAD_FILE:
      snprintf(fname, TSDB_FILENAME_LEN - 1, "%s%s%s%sv%df%dver%" PRId64 "%s", tfsGetDiskPath(pTfs, pDFileSet->diskId),
               TD_DIRSEP, pTsdb->path, TD_DIRSEP, TD_VID(pTsdb->pVnode), pDFileSet->fid, pDFileSet->fHead.commitID,
               ".head");
      break;
    case TSDB_DATA_FILE:
      snprintf(fname, TSDB_FILENAME_LEN - 1, "%s%s%s%sv%df%dver%" PRId64 "%s", tfsGetDiskPath(pTfs, pDFileSet->diskId),
               TD_DIRSEP, pTsdb->path, TD_DIRSEP, TD_VID(pTsdb->pVnode), pDFileSet->fid, pDFileSet->fData.commitID,
               ".data");
      break;
    case TSDB_LAST_FILE:
      snprintf(fname, TSDB_FILENAME_LEN - 1, "%s%s%s%sv%df%dver%" PRId64 "%s", tfsGetDiskPath(pTfs, pDFileSet->diskId),
               TD_DIRSEP, pTsdb->path, TD_DIRSEP, TD_VID(pTsdb->pVnode), pDFileSet->fid, pDFileSet->fLast.commitID,
               ".last");
      break;
    case TSDB_SMA_FILE:
      snprintf(fname, TSDB_FILENAME_LEN - 1, "%s%s%s%sv%df%dver%" PRId64 "%s", tfsGetDiskPath(pTfs, pDFileSet->diskId),
               TD_DIRSEP, pTsdb->path, TD_DIRSEP, TD_VID(pTsdb->pVnode), pDFileSet->fid, pDFileSet->fSma.commitID,
               ".sma");
      break;
    default:
      ASSERT(0);
      break;
  }
}

// SHeadFile ===============================================

// SDataFile ===============================================

// SLastFile ===============================================

// SSmaFile ===============================================

// SDelFile ===============================================
void tsdbDelFileName(STsdb *pTsdb, SDelFile *pFile, char fname[]) {
  STfs *pTfs = pTsdb->pVnode->pTfs;

  snprintf(fname, TSDB_FILENAME_LEN - 1, "%s%s%s%sv%dver%" PRId64 "%s", tfsGetPrimaryPath(pTfs), TD_DIRSEP, pTsdb->path,
           TD_DIRSEP, TD_VID(pTsdb->pVnode), pFile->commitID, ".del");
}