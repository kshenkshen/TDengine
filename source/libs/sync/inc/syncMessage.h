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

#ifndef _TD_LIBS_SYNC_MESSAGE_H
#define _TD_LIBS_SYNC_MESSAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "syncInt.h"

typedef enum ESyncTimeoutType {
  SYNC_TIMEOUT_PING = 100,
  SYNC_TIMEOUT_ELECTION,
  SYNC_TIMEOUT_HEARTBEAT,
} ESyncTimeoutType;

typedef struct SyncTimeout {
  uint32_t         bytes;
  int32_t          vgId;
  uint32_t         msgType;
  ESyncTimeoutType timeoutType;
  uint64_t         logicClock;
  int32_t          timerMS;
  void*            data;  // need optimized
} SyncTimeout;

typedef struct SyncClientRequest {
  uint32_t bytes;
  int32_t  vgId;
  uint32_t msgType;          // TDMT_SYNC_CLIENT_REQUEST
  uint32_t originalRpcType;  // origin RpcMsg msgType
  uint64_t seqNum;
  bool     isWeak;
  uint32_t dataLen;  // origin RpcMsg.contLen
  char     data[];   // origin RpcMsg.pCont
} SyncClientRequest;

typedef struct SyncClientRequestReply {
  uint32_t bytes;
  int32_t  vgId;
  uint32_t msgType;
  int32_t  errCode;
  SRaftId  leaderHint;
} SyncClientRequestReply;

typedef struct SyncRequestVote {
  uint32_t bytes;
  int32_t  vgId;
  uint32_t msgType;
  SRaftId  srcId;
  SRaftId  destId;
  // private data
  SyncTerm  term;
  SyncIndex lastLogIndex;
  SyncTerm  lastLogTerm;
} SyncRequestVote;

typedef struct SyncRequestVoteReply {
  uint32_t bytes;
  int32_t  vgId;
  uint32_t msgType;
  SRaftId  srcId;
  SRaftId  destId;
  // private data
  SyncTerm term;
  bool     voteGranted;
} SyncRequestVoteReply;

typedef struct SyncAppendEntries {
  uint32_t bytes;
  int32_t  vgId;
  uint32_t msgType;
  SRaftId  srcId;
  SRaftId  destId;

  // private data
  SyncTerm  term;
  SyncIndex prevLogIndex;
  SyncTerm  prevLogTerm;
  SyncIndex commitIndex;
  SyncTerm  privateTerm;
  uint32_t  dataLen;
  char      data[];
} SyncAppendEntries;

typedef struct SyncAppendEntriesReply {
  uint32_t bytes;
  int32_t  vgId;
  uint32_t msgType;
  SRaftId  srcId;
  SRaftId  destId;
  // private data
  SyncTerm  term;
  SyncTerm  privateTerm;
  bool      success;
  SyncIndex matchIndex;
  SyncIndex lastSendIndex;
  int64_t   startTime;
} SyncAppendEntriesReply;

typedef struct SyncHeartbeat {
  uint32_t bytes;
  int32_t  vgId;
  uint32_t msgType;
  SRaftId  srcId;
  SRaftId  destId;

  // private data
  SyncTerm  term;
  SyncIndex commitIndex;
  SyncTerm  privateTerm;
  SyncTerm  minMatchIndex;
} SyncHeartbeat;

typedef struct SyncHeartbeatReply {
  uint32_t bytes;
  int32_t  vgId;
  uint32_t msgType;
  SRaftId  srcId;
  SRaftId  destId;

  // private data
  SyncTerm term;
  SyncTerm privateTerm;
  int64_t  startTime;
} SyncHeartbeatReply;

typedef struct SyncPreSnapshot {
  uint32_t bytes;
  int32_t  vgId;
  uint32_t msgType;
  SRaftId  srcId;
  SRaftId  destId;

  // private data
  SyncTerm term;
} SyncPreSnapshot;

typedef struct SyncPreSnapshotReply {
  uint32_t bytes;
  int32_t  vgId;
  uint32_t msgType;
  SRaftId  srcId;
  SRaftId  destId;

  // private data
  SyncTerm  term;
  SyncIndex snapStart;
} SyncPreSnapshotReply;

typedef struct SyncApplyMsg {
  uint32_t   bytes;
  int32_t    vgId;
  uint32_t   msgType;          // user SyncApplyMsg msgType
  uint32_t   originalRpcType;  // user RpcMsg msgType
  SFsmCbMeta fsmMeta;
  uint32_t   dataLen;  // user RpcMsg.contLen
  char       data[];   // user RpcMsg.pCont
} SyncApplyMsg;

typedef struct SyncSnapshotSend {
  uint32_t bytes;
  int32_t  vgId;
  uint32_t msgType;
  SRaftId  srcId;
  SRaftId  destId;

  SyncTerm  term;
  SyncIndex beginIndex;       // snapshot.beginIndex
  SyncIndex lastIndex;        // snapshot.lastIndex
  SyncTerm  lastTerm;         // snapshot.lastTerm
  SyncIndex lastConfigIndex;  // snapshot.lastConfigIndex
  SSyncCfg  lastConfig;
  int64_t   startTime;
  int32_t   seq;
  uint32_t  dataLen;
  char      data[];
} SyncSnapshotSend;

typedef struct SyncSnapshotRsp {
  uint32_t bytes;
  int32_t  vgId;
  uint32_t msgType;
  SRaftId  srcId;
  SRaftId  destId;

  SyncTerm  term;
  SyncIndex lastIndex;
  SyncTerm  lastTerm;
  int64_t   startTime;
  int32_t   ack;
  int32_t   code;
  SyncIndex snapBeginIndex;  // when ack = SYNC_SNAPSHOT_SEQ_BEGIN, it's valid
} SyncSnapshotRsp;

typedef struct SyncLeaderTransfer {
  uint32_t bytes;
  int32_t  vgId;
  uint32_t msgType;
  /*
   SRaftId  srcId;
   SRaftId  destId;
   */
  SNodeInfo newNodeInfo;
  SRaftId   newLeaderId;
} SyncLeaderTransfer;

typedef enum {
  SYNC_LOCAL_CMD_STEP_DOWN = 100,
  SYNC_LOCAL_CMD_FOLLOWER_CMT,
} ESyncLocalCmd;

typedef struct SyncLocalCmd {
  uint32_t bytes;
  int32_t  vgId;
  uint32_t msgType;
  SRaftId  srcId;
  SRaftId  destId;

  int32_t   cmd;
  SyncTerm  sdNewTerm;  // step down new term
  SyncIndex fcIndex;    // follower commit index
} SyncLocalCmd;

int32_t syncBuildTimeout(SRpcMsg* pMsg, ESyncTimeoutType ttype, uint64_t logicClock, int32_t ms, SSyncNode* pNode);
int32_t syncBuildClientRequest(SRpcMsg* pMsg, const SRpcMsg* pOriginal, uint64_t seq, bool isWeak, int32_t vgId);
int32_t syncBuildClientRequestFromNoopEntry(SRpcMsg* pMsg, const SSyncRaftEntry* pEntry, int32_t vgId);
int32_t syncBuildRequestVote(SRpcMsg* pMsg, int32_t vgId);
int32_t syncBuildRequestVoteReply(SRpcMsg* pMsg, int32_t vgId);
int32_t syncBuildAppendEntries(SRpcMsg* pMsg, int32_t dataLen, int32_t vgId);
int32_t syncBuildAppendEntriesReply(SRpcMsg* pMsg, int32_t vgId);
int32_t syncBuildHeartbeat(SRpcMsg* pMsg, int32_t vgId);
int32_t syncBuildHeartbeatReply(SRpcMsg* pMsg, int32_t vgId);
int32_t syncBuildPreSnapshot(SRpcMsg* pMsg, int32_t vgId);
int32_t syncBuildPreSnapshotReply(SRpcMsg* pMsg, int32_t vgId);
int32_t syncBuildApplyMsg(SRpcMsg* pMsg, const SRpcMsg* pOriginal, int32_t vgId, SFsmCbMeta* pMeta);
int32_t syncBuildSnapshotSend(SRpcMsg* pMsg, int32_t dataLen, int32_t vgId);
int32_t syncBuildSnapshotSendRsp(SRpcMsg* pMsg, int32_t vgId);
int32_t syncBuildLeaderTransfer(SRpcMsg* pMsg, int32_t vgId);
int32_t syncBuildLocalCmd(SRpcMsg* pMsg, int32_t vgId);

const char* syncTimerTypeStr(ESyncTimeoutType timerType);
const char* syncLocalCmdGetStr(ESyncLocalCmd cmd);

#ifdef __cplusplus
}
#endif

#endif /*_TD_LIBS_SYNC_MESSAGE_H*/
