/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2019 Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * Mocks - protocol request handlers
 *
 */

#include "rdkafka_int.h"
#include "rdbuf.h"
#include "rdrand.h"
#include "rdkafka_interceptor.h"
#include "rdkafka_mock_int.h"
#include "rdkafka_transport_int.h"
#include "rdkafka_offset.h"




/**
 * @brief Handle ProduceRequest
 */
static int rd_kafka_mock_handle_Produce (rd_kafka_mock_connection_t *mconn,
                                         rd_kafka_buf_t *rkbuf) {
        const rd_bool_t log_decode_errors = rd_true;
        rd_kafka_mock_cluster_t *mcluster = mconn->broker->cluster;
        rd_kafka_buf_t *resp = rd_kafka_mock_buf_new_response(rkbuf);
        int32_t TopicsCnt;
        rd_kafkap_str_t TransactionalId = RD_KAFKAP_STR_INITIALIZER;
        int16_t Acks;
        int32_t TimeoutMs;
        rd_kafka_resp_err_t all_err;

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 3)
                rd_kafka_buf_read_str(rkbuf, &TransactionalId);

        rd_kafka_buf_read_i16(rkbuf, &Acks);
        rd_kafka_buf_read_i32(rkbuf, &TimeoutMs);
        rd_kafka_buf_read_i32(rkbuf, &TopicsCnt);

        /* Response: #Topics */
        rd_kafka_buf_write_i32(resp, TopicsCnt);

        /* Inject error, if any */
        all_err = rd_kafka_mock_next_request_error(mcluster,
                                                   rkbuf->rkbuf_reqhdr.ApiKey);

        while (TopicsCnt-- > 0) {
                rd_kafkap_str_t Topic;
                int32_t PartitionCnt;
                rd_kafka_mock_topic_t *mtopic;

                rd_kafka_buf_read_str(rkbuf, &Topic);
                rd_kafka_buf_read_i32(rkbuf, &PartitionCnt);

                mtopic = rd_kafka_mock_topic_find_by_kstr(mcluster, &Topic);

                /* Response: Topic */
                rd_kafka_buf_write_kstr(resp, &Topic);
                /* Response: #Partitions */
                rd_kafka_buf_write_i32(resp, PartitionCnt);

                while (PartitionCnt-- > 0) {
                        int32_t Partition;
                        rd_kafka_mock_partition_t *mpart = NULL;
                        rd_kafkap_bytes_t records;
                        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
                        int64_t BaseOffset = -1;

                        rd_kafka_buf_read_i32(rkbuf, &Partition);

                        if (mtopic)
                                mpart = rd_kafka_mock_partition_find(mtopic,
                                                                     Partition);

                        rd_kafka_buf_read_bytes(rkbuf, &records);

                        /* Response: Partition */
                        rd_kafka_buf_write_i32(resp, Partition);

                        if (all_err)
                                err = all_err;
                        else if (!mpart)
                                err = RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART;
                        else if (mpart->leader != mconn->broker)
                                err = RD_KAFKA_RESP_ERR_NOT_LEADER_FOR_PARTITION;

                        /* Append to partition log */
                        if (!err)
                                err = rd_kafka_mock_partition_log_append(
                                        mpart, &records, &BaseOffset);

                        /* Response: ErrorCode */
                        rd_kafka_buf_write_i16(resp, err);

                        if (err) {
                                /* Response: BaseOffset */
                                rd_kafka_buf_write_i64(resp, BaseOffset);

                                if (rkbuf->rkbuf_reqhdr.ApiVersion >= 2) {
                                        /* Response: LogAppendTimeMs */
                                        rd_kafka_buf_write_i64(resp, -1);
                                }
                                if (rkbuf->rkbuf_reqhdr.ApiVersion >= 6) {
                                        /* Response: LogStartOffset */
                                        rd_kafka_buf_write_i64(resp, -1);
                                }

                        } else {
                                /* Response: BaseOffset */
                                rd_kafka_buf_write_i64(resp, BaseOffset);

                                if (rkbuf->rkbuf_reqhdr.ApiVersion >= 2) {
                                        /* Response: LogAppendTimeMs */
                                        rd_kafka_buf_write_i64(resp, 1234);
                                }
                                if (rkbuf->rkbuf_reqhdr.ApiVersion >= 6) {
                                        /* Response: LogStartOffset */
                                        rd_kafka_buf_write_i64(
                                                resp, mpart->start_offset);
                                }
                        }
                }
        }

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 1) {
                /* Response: ThrottleTime */
                rd_kafka_buf_write_i32(resp, 0);
        }

        rd_kafka_mock_connection_send_response(mconn, rkbuf, resp);

        return 0;

 err_parse:
        rd_kafka_buf_destroy(resp);
        return -1;
}



/**
 * @brief Handle FetchRequest
 */
static int rd_kafka_mock_handle_Fetch (rd_kafka_mock_connection_t *mconn,
                                       rd_kafka_buf_t *rkbuf) {
        const rd_bool_t log_decode_errors = rd_true;
        rd_kafka_mock_cluster_t *mcluster = mconn->broker->cluster;
        rd_kafka_buf_t *resp = rd_kafka_mock_buf_new_response(rkbuf);
        rd_kafka_resp_err_t all_err;
        int32_t ReplicaId, MaxWait, MinBytes, MaxBytes = -1, SessionId = -1,
                Epoch, TopicsCnt;
        int8_t IsolationLevel;
        size_t totsize = 0;

        rd_kafka_buf_read_i32(rkbuf, &ReplicaId);
        rd_kafka_buf_read_i32(rkbuf, &MaxWait);
        rd_kafka_buf_read_i32(rkbuf, &MinBytes);
        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 3)
                rd_kafka_buf_read_i32(rkbuf, &MaxBytes);
        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 4)
                rd_kafka_buf_read_i8(rkbuf, &IsolationLevel);
        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 7) {
                rd_kafka_buf_read_i32(rkbuf, &SessionId);
                rd_kafka_buf_read_i32(rkbuf, &Epoch);
        }

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 1) {
                /* Response: ThrottleTime */
                rd_kafka_buf_write_i32(resp, 0);
        }


        /* Inject error, if any */
        all_err = rd_kafka_mock_next_request_error(mcluster,
                                                   rkbuf->rkbuf_reqhdr.ApiKey);

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 7) {
                /* Response: ErrorCode */
                rd_kafka_buf_write_i16(resp, all_err);

                /* Response: SessionId */
                rd_kafka_buf_write_i32(resp, SessionId);
        }

        rd_kafka_buf_read_i32(rkbuf, &TopicsCnt);

        /* Response: #Topics */
        rd_kafka_buf_write_i32(resp, TopicsCnt);

        while (TopicsCnt-- > 0) {
                rd_kafkap_str_t Topic;
                int32_t PartitionCnt;
                rd_kafka_mock_topic_t *mtopic;

                rd_kafka_buf_read_str(rkbuf, &Topic);
                rd_kafka_buf_read_i32(rkbuf, &PartitionCnt);

                mtopic = rd_kafka_mock_topic_find_by_kstr(mcluster, &Topic);

                /* Response: Topic */
                rd_kafka_buf_write_kstr(resp, &Topic);
                /* Response: #Partitions */
                rd_kafka_buf_write_i32(resp, PartitionCnt);

                while (PartitionCnt-- > 0) {
                        int32_t Partition, CurrentLeaderEpoch, PartMaxBytes;
                        int64_t FetchOffset, LogStartOffset;
                        rd_kafka_mock_partition_t *mpart = NULL;
                        rd_kafka_resp_err_t err = all_err;
                        rd_bool_t on_follower;
                        size_t partsize = 0;
                        const rd_kafka_mock_msgset_t *mset = NULL;

                        rd_kafka_buf_read_i32(rkbuf, &Partition);

                        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 9)
                                rd_kafka_buf_read_i32(rkbuf,
                                                      &CurrentLeaderEpoch);

                        rd_kafka_buf_read_i64(rkbuf, &FetchOffset);

                        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 5)
                                rd_kafka_buf_read_i64(rkbuf, &LogStartOffset);

                        rd_kafka_buf_read_i32(rkbuf, &PartMaxBytes);

                        if (mtopic)
                                mpart = rd_kafka_mock_partition_find(mtopic,
                                                                     Partition);

                        /* Response: Partition */
                        rd_kafka_buf_write_i32(resp, Partition);

                        /* Fetch is directed at follower and this is
                         * the follower broker. */
                        on_follower = mpart &&
                                mpart->follower_id == mconn->broker->id;

                        if (!all_err && !mpart)
                                err = RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART;
                        else if (!all_err &&
                                 mpart->leader != mconn->broker &&
                                 !on_follower)
                                err = RD_KAFKA_RESP_ERR_NOT_LEADER_FOR_PARTITION;

                        /* Find MessageSet for FetchOffset */
                        if (!err && FetchOffset != mpart->end_offset) {
                                if (on_follower &&
                                    FetchOffset <= mpart->end_offset &&
                                    FetchOffset > mpart->follower_end_offset)
                                        err = RD_KAFKA_RESP_ERR_OFFSET_NOT_AVAILABLE;
                                else if (!(mset = rd_kafka_mock_msgset_find(
                                                   mpart,
                                                   FetchOffset,
                                                   on_follower)))
                                        err = RD_KAFKA_RESP_ERR_OFFSET_OUT_OF_RANGE;
                        }


                        /* Response: ErrorCode */
                        rd_kafka_buf_write_i16(resp, err);

                        /* Response: Highwatermark */
                        rd_kafka_buf_write_i64(resp,
                                               mpart ?
                                               (on_follower ?
                                                mpart->follower_end_offset :
                                                mpart->end_offset) : -1);

                        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 4) {
                                /* Response: LastStableOffset */
                                rd_kafka_buf_write_i64(resp,
                                                       mpart ?
                                                       mpart->end_offset : -1);
                        }

                        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 5) {
                                /* Response: LogStartOffset */
                                rd_kafka_buf_write_i64(
                                        resp,
                                        !mpart ? -1 :
                                        (on_follower ?
                                         mpart->follower_start_offset :
                                         mpart->start_offset));
                        }

                        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 4) {
                                /* Response: #Aborted */
                                rd_kafka_buf_write_i32(resp, 0);
                        }


                        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 11) {
                                int32_t PreferredReadReplica =
                                        mpart &&
                                        mpart->leader == mconn->broker &&
                                        mpart->follower_id != -1 ?
                                        mpart->follower_id : -1;

                                /* Response: PreferredReplica */
                                rd_kafka_buf_write_i32(
                                        resp, PreferredReadReplica);

                                if (PreferredReadReplica != -1) {
                                        /* Don't return any data when
                                         * PreferredReadReplica is set */
                                        mset = NULL;
                                        MaxWait = 0;
                                }
                        }


                        if (mset &&
                            partsize < (size_t)PartMaxBytes &&
                            totsize < (size_t)MaxBytes) {
                                rd_kafka_dbg(mcluster->rk, MOCK, "MOCK",
                                             "Broker %"PRId32": "
                                             "Topic %s [%"PRId32"]: "
                                             "fetch response at "
                                             "Offset %"PRId64
                                             " (requested Offset %"PRId64"): "
                                             "MessageSet of %"PRId32" bytes",
                                             mconn->broker->id,
                                             mtopic->name, mpart->id,
                                             mset->first_offset, FetchOffset,
                                             RD_KAFKAP_BYTES_SIZE(&mset->
                                                                  bytes));
                                /* Response: Records */
                                rd_kafka_buf_write_kbytes(resp, &mset->bytes);
                                partsize += RD_KAFKAP_BYTES_SIZE(&mset->bytes);
                                totsize += RD_KAFKAP_BYTES_SIZE(&mset->bytes);

                                /* FIXME: Multiple messageSets ? */
                        } else {
                                rd_kafka_dbg(mcluster->rk, MOCK, "MOCK",
                                             "Broker %"PRId32": "
                                             "Topic %s [%"PRId32"]: empty "
                                             "fetch response for requested "
                                             "Offset %"PRId64": "
                                             "Log start..end Offsets are "
                                             "%"PRId64"..%"PRId64
                                             " (follower %"PRId64"..%"PRId64")",
                                             mconn->broker->id,
                                             mtopic ? mtopic->name : "n/a",
                                             mpart ? mpart->id : -1,
                                             FetchOffset,
                                             mpart ? mpart->start_offset : -1,
                                             mpart ? mpart->end_offset : -1,
                                             mpart ?
                                             mpart->follower_start_offset : -1,
                                             mpart ?
                                             mpart->follower_end_offset : -1);
                                /* Response: Records: Null */
                                rd_kafka_buf_write_i32(resp, 0);
                        }
                }
        }

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 7) {
                int32_t ForgottenTopicCnt;
                rd_kafka_buf_read_i32(rkbuf, &ForgottenTopicCnt);
                while (ForgottenTopicCnt-- > 0) {
                        rd_kafkap_str_t Topic;
                        int32_t ForgPartCnt;
                        rd_kafka_buf_read_str(rkbuf, &Topic);
                        rd_kafka_buf_read_i32(rkbuf, &ForgPartCnt);
                        while (ForgPartCnt-- > 0) {
                                int32_t Partition;
                                rd_kafka_buf_read_i32(rkbuf, &Partition);
                        }
                }
        }

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 11) {
                rd_kafkap_str_t RackId;
                char *rack;
                rd_kafka_buf_read_str(rkbuf, &RackId);
                RD_KAFKAP_STR_DUPA(&rack, &RackId);
                /* Matt might do something sensible with this */
        }

        /* If there was no data, delay up to MaxWait.
         * This isn't strictly correct since we should cut the wait short
         * and feed newly produced data if a producer writes to the
         * partitions, but that is too much of a hassle here since we
         * can't block the thread. */
        if (!totsize && MaxWait > 0)
                resp->rkbuf_ts_retry = rd_clock() + (MaxWait * 1000);

        rd_kafka_mock_connection_send_response(mconn, rkbuf, resp);

        return 0;

 err_parse:
        rd_kafka_buf_destroy(resp);
        return -1;
}




/**
 * @brief Handle ListOffset
 */
static int rd_kafka_mock_handle_ListOffset (rd_kafka_mock_connection_t *mconn,
                                            rd_kafka_buf_t *rkbuf) {
        const rd_bool_t log_decode_errors = rd_true;
        rd_kafka_mock_cluster_t *mcluster = mconn->broker->cluster;
        rd_kafka_buf_t *resp = rd_kafka_mock_buf_new_response(rkbuf);
        rd_kafka_resp_err_t all_err;
        int32_t ReplicaId, TopicsCnt;
        int8_t IsolationLevel;

        rd_kafka_buf_read_i32(rkbuf, &ReplicaId);
        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 2)
                rd_kafka_buf_read_i8(rkbuf, &IsolationLevel);

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 2) {
                /* Response: ThrottleTime */
                rd_kafka_buf_write_i32(resp, 0);
        }


        /* Inject error, if any */
        all_err = rd_kafka_mock_next_request_error(mcluster,
                                                   rkbuf->rkbuf_reqhdr.ApiKey);

        rd_kafka_buf_read_i32(rkbuf, &TopicsCnt);

        /* Response: #Topics */
        rd_kafka_buf_write_i32(resp, TopicsCnt);

        while (TopicsCnt-- > 0) {
                rd_kafkap_str_t Topic;
                int32_t PartitionCnt;
                rd_kafka_mock_topic_t *mtopic;

                rd_kafka_buf_read_str(rkbuf, &Topic);
                rd_kafka_buf_read_i32(rkbuf, &PartitionCnt);

                mtopic = rd_kafka_mock_topic_find_by_kstr(mcluster, &Topic);

                /* Response: Topic */
                rd_kafka_buf_write_kstr(resp, &Topic);
                /* Response: #Partitions */
                rd_kafka_buf_write_i32(resp, PartitionCnt);

                while (PartitionCnt-- > 0) {
                        int32_t Partition, CurrentLeaderEpoch;
                        int64_t Timestamp, MaxNumOffsets, Offset = -1;
                        rd_kafka_mock_partition_t *mpart = NULL;
                        rd_kafka_resp_err_t err = all_err;

                        rd_kafka_buf_read_i32(rkbuf, &Partition);

                        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 4)
                                rd_kafka_buf_read_i32(rkbuf,
                                                      &CurrentLeaderEpoch);

                        rd_kafka_buf_read_i64(rkbuf, &Timestamp);
                        rd_kafka_buf_read_i32(rkbuf, &MaxNumOffsets);

                        if (mtopic)
                                mpart = rd_kafka_mock_partition_find(mtopic,
                                                                     Partition);

                        /* Response: Partition */
                        rd_kafka_buf_write_i32(resp, Partition);

                        if (!all_err && !mpart)
                                err = RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART;
                        else if (!all_err &&
                                 mpart->leader != mconn->broker)
                                err = RD_KAFKA_RESP_ERR_NOT_LEADER_FOR_PARTITION;


                        /* Response: ErrorCode */
                        rd_kafka_buf_write_i16(resp, err);

                        if (!err && mpart) {
                                if (Timestamp == RD_KAFKA_OFFSET_BEGINNING)
                                        Offset = mpart->start_offset;
                                else if (Timestamp == RD_KAFKA_OFFSET_END)
                                        Offset = mpart->end_offset;
                                else if (Timestamp < 0)
                                        Offset = -1;
                                else /* FIXME: by timestamp */
                                        Offset = -1;
                        }

                        if (rkbuf->rkbuf_reqhdr.ApiVersion == 0) {
                                /* Response: #OldStyleOffsets */
                                rd_kafka_buf_write_i32(resp,
                                                       Offset != -1 ? 1 : 0);
                                /* Response: OldStyleOffsets[0] */
                                if (Offset != -1)
                                        rd_kafka_buf_write_i64(resp, Offset);
                        } else {
                                /* Response: Offset */
                                rd_kafka_buf_write_i64(resp, Offset);
                        }


                        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 1) {
                                /* Response: Timestamp (FIXME) */
                                rd_kafka_buf_write_i64(resp, -1);
                        }

                        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 4) {
                                /* Response: LeaderEpoch */
                                rd_kafka_buf_write_i64(resp, -1);
                        }

                        rd_kafka_dbg(mcluster->rk, MOCK, "MOCK",
                                     "Topic %.*s [%"PRId32"] returning "
                                     "offset %"PRId64" for %s: %s",
                                     RD_KAFKAP_STR_PR(&Topic),
                                     Partition,
                                     Offset,
                                     rd_kafka_offset2str(Timestamp),
                                     rd_kafka_err2str(err));

                }
        }


        rd_kafka_mock_connection_send_response(mconn, rkbuf, resp);

        return 0;

 err_parse:
        rd_kafka_buf_destroy(resp);
        return -1;
}


/**
 * @brief Handle OffsetFetch (fetch committed offsets)
 */
static int rd_kafka_mock_handle_OffsetFetch (rd_kafka_mock_connection_t *mconn,
                                             rd_kafka_buf_t *rkbuf) {
        const rd_bool_t log_decode_errors = rd_true;
        rd_kafka_mock_cluster_t *mcluster = mconn->broker->cluster;
        rd_kafka_buf_t *resp = rd_kafka_mock_buf_new_response(rkbuf);
        rd_kafka_mock_broker_t *mrkb;
        rd_kafka_resp_err_t all_err;
        int32_t TopicsCnt;
        rd_kafkap_str_t GroupId;

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 3) {
                /* Response: ThrottleTime */
                rd_kafka_buf_write_i32(resp, 0);
        }

        rd_kafka_buf_read_str(rkbuf, &GroupId);

        /* Inject error, if any */
        all_err = rd_kafka_mock_next_request_error(mcluster,
                                                   rkbuf->rkbuf_reqhdr.ApiKey);

        mrkb = rd_kafka_mock_cluster_get_coord(mcluster, RD_KAFKA_COORD_GROUP,
                                               &GroupId);
        if (!mrkb && !all_err)
                all_err = RD_KAFKA_RESP_ERR_NOT_COORDINATOR;


        rd_kafka_buf_read_i32(rkbuf, &TopicsCnt);

        /* Response: #Topics */
        rd_kafka_buf_write_i32(resp, TopicsCnt);

        while (TopicsCnt-- > 0) {
                rd_kafkap_str_t Topic;
                int32_t PartitionCnt;
                rd_kafka_mock_topic_t *mtopic;

                rd_kafka_buf_read_str(rkbuf, &Topic);
                rd_kafka_buf_read_i32(rkbuf, &PartitionCnt);

                mtopic = rd_kafka_mock_topic_find_by_kstr(mcluster, &Topic);

                /* Response: Topic */
                rd_kafka_buf_write_kstr(resp, &Topic);
                /* Response: #Partitions */
                rd_kafka_buf_write_i32(resp, PartitionCnt);

                while (PartitionCnt-- > 0) {
                        int32_t Partition;
                        rd_kafka_mock_partition_t *mpart = NULL;
                        const rd_kafka_mock_committed_offset_t *coff = NULL;
                        rd_kafka_resp_err_t err = all_err;

                        rd_kafka_buf_read_i32(rkbuf, &Partition);

                        if (mtopic)
                                mpart = rd_kafka_mock_partition_find(mtopic,
                                                                     Partition);

                        /* Response: Partition */
                        rd_kafka_buf_write_i32(resp, Partition);

                        if (!all_err && !mpart)
                                err = RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART;

                        if (!err)
                                coff = rd_kafka_mock_committed_offset_find(
                                        mpart, &GroupId);

                        /* Response: CommittedOffset */
                        rd_kafka_buf_write_i64(resp, coff ? coff->offset : -1);

                        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 5) {
                                /* Response: CommittedLeaderEpoch */
                                rd_kafka_buf_write_i32(resp, -1);
                        }

                        /* Response: Metadata */
                        rd_kafka_buf_write_kstr(resp,
                                                coff ? coff->metadata : NULL);

                        /* Response: ErrorCode */
                        rd_kafka_buf_write_i16(resp, err);

                        if (coff)
                                rd_kafka_dbg(mcluster->rk, MOCK, "MOCK",
                                             "Topic %s [%"PRId32"] returning "
                                             "committed offset %"PRId64
                                             " for group %s",
                                             mtopic->name, mpart->id,
                                             coff->offset, coff->group);
                        else
                                rd_kafka_dbg(mcluster->rk, MOCK, "MOCK",
                                             "Topic %.*s [%"PRId32"] has no "
                                             "committed offset for group %.*s: "
                                             "%s",
                                             RD_KAFKAP_STR_PR(&Topic),
                                             Partition,
                                             RD_KAFKAP_STR_PR(&GroupId),
                                             rd_kafka_err2str(err));
                }
        }

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 2) {
                /* Response: Outer ErrorCode */
                rd_kafka_buf_write_i16(resp, all_err);
        }


        rd_kafka_mock_connection_send_response(mconn, rkbuf, resp);

        return 0;

 err_parse:
        rd_kafka_buf_destroy(resp);
        return -1;
}



/**
 * @brief Handle OffsetCommit
 */
static int rd_kafka_mock_handle_OffsetCommit (rd_kafka_mock_connection_t *mconn,
                                              rd_kafka_buf_t *rkbuf) {
        const rd_bool_t log_decode_errors = rd_true;
        rd_kafka_mock_cluster_t *mcluster = mconn->broker->cluster;
        rd_kafka_buf_t *resp = rd_kafka_mock_buf_new_response(rkbuf);
        rd_kafka_mock_broker_t *mrkb;
        rd_kafka_resp_err_t all_err;
        int32_t GenerationId, TopicsCnt;
        rd_kafkap_str_t GroupId, MemberId, GroupInstanceId;

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 3) {
                /* Response: ThrottleTime */
                rd_kafka_buf_write_i32(resp, 0);
        }

        rd_kafka_buf_read_str(rkbuf, &GroupId);

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 1) {
                rd_kafka_buf_read_i32(rkbuf, &GenerationId);
                rd_kafka_buf_read_str(rkbuf, &MemberId);
        }

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 7)
                rd_kafka_buf_read_str(rkbuf, &GroupInstanceId);

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 2 &&
            rkbuf->rkbuf_reqhdr.ApiVersion <= 4) {
                int64_t RetentionTimeMs;
                rd_kafka_buf_read_i64(rkbuf, &RetentionTimeMs);
        }


        /* Inject error, if any */
        all_err = rd_kafka_mock_next_request_error(mcluster,
                                                   rkbuf->rkbuf_reqhdr.ApiKey);

        mrkb = rd_kafka_mock_cluster_get_coord(mcluster, RD_KAFKA_COORD_GROUP,
                                               &GroupId);
        if (!mrkb && !all_err)
                all_err = RD_KAFKA_RESP_ERR_NOT_COORDINATOR;

        rd_kafka_buf_read_i32(rkbuf, &TopicsCnt);

        /* Response: #Topics */
        rd_kafka_buf_write_i32(resp, TopicsCnt);

        while (TopicsCnt-- > 0) {
                rd_kafkap_str_t Topic;
                int32_t PartitionCnt;
                rd_kafka_mock_topic_t *mtopic;

                rd_kafka_buf_read_str(rkbuf, &Topic);
                rd_kafka_buf_read_i32(rkbuf, &PartitionCnt);

                mtopic = rd_kafka_mock_topic_find_by_kstr(mcluster, &Topic);

                /* Response: Topic */
                rd_kafka_buf_write_kstr(resp, &Topic);
                /* Response: #Partitions */
                rd_kafka_buf_write_i32(resp, PartitionCnt);

                while (PartitionCnt-- > 0) {
                        int32_t Partition;
                        rd_kafka_mock_partition_t *mpart = NULL;
                        rd_kafka_resp_err_t err = all_err;
                        int64_t CommittedOffset;
                        rd_kafkap_str_t Metadata;

                        rd_kafka_buf_read_i32(rkbuf, &Partition);

                        if (mtopic)
                                mpart = rd_kafka_mock_partition_find(mtopic,
                                                                     Partition);

                        /* Response: Partition */
                        rd_kafka_buf_write_i32(resp, Partition);

                        if (!all_err && !mpart)
                                err = RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART;

                        rd_kafka_buf_read_i64(rkbuf, &CommittedOffset);

                        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 6) {
                                int32_t CommittedLeaderEpoch;
                                rd_kafka_buf_read_i32(rkbuf,
                                                      &CommittedLeaderEpoch);
                        }

                        if (rkbuf->rkbuf_reqhdr.ApiVersion == 1) {
                                int64_t CommitTimestamp;
                                rd_kafka_buf_read_i64(rkbuf, &CommitTimestamp);
                        }

                        rd_kafka_buf_read_str(rkbuf, &Metadata);

                        if (!err)
                                rd_kafka_mock_commit_offset(
                                        mpart, &GroupId, CommittedOffset,
                                        &Metadata);

                        /* Response: ErrorCode */
                        rd_kafka_buf_write_i16(resp, err);
                }
        }

        rd_kafka_mock_connection_send_response(mconn, rkbuf, resp);

        return 0;

 err_parse:
        rd_kafka_buf_destroy(resp);
        return -1;
}




/**
 * @brief Handle ApiVersionRequest
 */
static int rd_kafka_mock_handle_ApiVersion (rd_kafka_mock_connection_t *mconn,
                                            rd_kafka_buf_t *rkbuf);


/**
 * @brief Write a MetadataResponse.Topics. entry to \p resp.
 *
 * @param mtopic may be NULL
 */
static void
rd_kafka_mock_buf_write_Metadata_Topic (rd_kafka_buf_t *resp,
                                        int16_t ApiVersion,
                                        const char *topic,
                                        const rd_kafka_mock_topic_t *mtopic,
                                        rd_kafka_resp_err_t err) {
        int i;

        /* Response: Topics.ErrorCode */
        rd_kafka_buf_write_i16(resp, err);
        /* Response: Topics.Name */
        rd_kafka_buf_write_str(resp, topic, -1);
        if (ApiVersion >= 1) {
                /* Response: Topics.IsInternal */
                rd_kafka_buf_write_bool(resp, rd_false);
        }
        /* Response: Topics.#Partitions */
        rd_kafka_buf_write_i32(resp, mtopic ? mtopic->partition_cnt : 0);

        for (i = 0 ; mtopic && i < mtopic->partition_cnt ; i++) {
                const rd_kafka_mock_partition_t *mpart =
                        &mtopic->partitions[i];
                int r;

                /* Response: ..Partitions.ErrorCode */
                rd_kafka_buf_write_i16(resp, 0);
                /* Response: ..Partitions.PartitionIndex */
                rd_kafka_buf_write_i32(resp, mpart->id);
                /* Response: ..Partitions.Leader */
                rd_kafka_buf_write_i32(resp,
                                       mpart->leader ?
                                       mpart->leader->id : -1);

                if (ApiVersion >= 7) {
                        /* Response: ..Partitions.LeaderEpoch */
                        rd_kafka_buf_write_i32(resp, -1);
                }

                /* Response: ..Partitions.#ReplicaNodes */
                rd_kafka_buf_write_i32(resp,
                                       mpart->replica_cnt);
                for (r = 0 ; r < mpart->replica_cnt ; r++)
                        rd_kafka_buf_write_i32(
                                resp,
                                mpart->replicas[r]->id);

                /* Response: ..Partitions.#IsrNodes */
                /* Let Replicas == ISRs for now */
                rd_kafka_buf_write_i32(resp,
                                       mpart->replica_cnt);
                for (r = 0 ; r < mpart->replica_cnt ; r++)
                        rd_kafka_buf_write_i32(
                                resp,
                                mpart->replicas[r]->id);

                if (ApiVersion >= 5) {
                        /* Response: ...OfflineReplicas */
                        rd_kafka_buf_write_i32(resp, 0);
                }
        }
}


/**
 * @brief Handle MetadataRequest
 */
static int rd_kafka_mock_handle_Metadata (rd_kafka_mock_connection_t *mconn,
                                          rd_kafka_buf_t *rkbuf) {
        const rd_bool_t log_decode_errors = rd_true;
        rd_kafka_mock_cluster_t *mcluster = mconn->broker->cluster;
        rd_bool_t AllowAutoTopicCreation = rd_true;
        rd_kafka_buf_t *resp = rd_kafka_mock_buf_new_response(rkbuf);
        const rd_kafka_mock_broker_t *mrkb;
        rd_kafka_topic_partition_list_t *requested_topics = NULL;
        rd_bool_t list_all_topics = rd_false;
        rd_kafka_mock_topic_t *mtopic;
        int32_t TopicsCnt;
        int i;

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 3) {
                /* Response: ThrottleTime */
                rd_kafka_buf_write_i32(resp, 0);
        }

        /* Response: #Brokers */
        rd_kafka_buf_write_i32(resp, mcluster->broker_cnt);

        TAILQ_FOREACH(mrkb, &mcluster->brokers, link) {
                /* Response: Brokers.Nodeid */
                rd_kafka_buf_write_i32(resp, mrkb->id);
                /* Response: Brokers.Host */
                rd_kafka_buf_write_str(resp, mrkb->advertised_listener, -1);
                /* Response: Brokers.Port */
                rd_kafka_buf_write_i32(resp, mrkb->port);
                if (rkbuf->rkbuf_reqhdr.ApiVersion >= 1) {
                        /* Response: Brokers.Rack (Matt's going to love this) */
                        rd_kafka_buf_write_str(resp, mrkb->rack, -1);
                }
        }

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 2) {
                /* Response: ClusterId */
                rd_kafka_buf_write_str(resp, mcluster->id, -1);
        }

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 1) {
                /* Response: ControllerId */
                rd_kafka_buf_write_i32(resp, mcluster->controller_id);
        }

        /* #Topics */
        rd_kafka_buf_read_i32(rkbuf, &TopicsCnt);

        if (TopicsCnt > 0)
                requested_topics = rd_kafka_topic_partition_list_new(TopicsCnt);
        else if (rkbuf->rkbuf_reqhdr.ApiVersion == 0 || TopicsCnt == -1)
                list_all_topics = rd_true;

        for (i = 0 ; i < TopicsCnt ; i++) {
                rd_kafkap_str_t Topic;
                char *topic;

                rd_kafka_buf_read_str(rkbuf, &Topic);
                RD_KAFKAP_STR_DUPA(&topic, &Topic);

                rd_kafka_topic_partition_list_add(requested_topics, topic,
                                                  RD_KAFKA_PARTITION_UA);
        }

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 4)
                rd_kafka_buf_read_bool(rkbuf, &AllowAutoTopicCreation);

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 8) {
                rd_bool_t IncludeClusterAuthorizedOperations;
                rd_bool_t IncludeTopicAuthorizedOperations;
                rd_kafka_buf_read_bool(rkbuf,
                                       &IncludeClusterAuthorizedOperations);
                rd_kafka_buf_read_bool(rkbuf,
                                       &IncludeTopicAuthorizedOperations);
        }

        if (list_all_topics) {
                /* Response: #Topics */
                rd_kafka_buf_write_i32(resp, mcluster->topic_cnt);

                TAILQ_FOREACH(mtopic, &mcluster->topics, link) {
                        rd_kafka_mock_buf_write_Metadata_Topic(
                                resp, rkbuf->rkbuf_reqhdr.ApiVersion,
                                mtopic->name, mtopic,
                                RD_KAFKA_RESP_ERR_NO_ERROR);
                }

        } else if (requested_topics) {
                /* Response: #Topics */
                rd_kafka_buf_write_i32(resp, requested_topics->cnt);

                for (i = 0 ; i < requested_topics->cnt ; i++) {
                        const rd_kafka_topic_partition_t *rktpar =
                                &requested_topics->elems[i];
                        rd_kafka_mock_topic_t *mtopic;
                        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;

                        mtopic = rd_kafka_mock_topic_find(mcluster,
                                                          rktpar->topic);
                        if (!mtopic && AllowAutoTopicCreation)
                                mtopic = rd_kafka_mock_topic_auto_create(
                                        mcluster, rktpar->topic, -1, &err);
                        else if (!mtopic)
                                err = RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART;

                        rd_kafka_mock_buf_write_Metadata_Topic(
                                resp, rkbuf->rkbuf_reqhdr.ApiVersion,
                                rktpar->topic, mtopic, err);
                }

                if (rkbuf->rkbuf_reqhdr.ApiVersion >= 8) {
                        /* TopicAuthorizedOperations */
                        rd_kafka_buf_write_i32(resp, INT32_MIN);
                }
        } else {
                /* Response: #Topics: brokers only */
                rd_kafka_buf_write_i32(resp, 0);
        }

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 8) {
                /* ClusterAuthorizedOperations */
                rd_kafka_buf_write_i32(resp, INT32_MIN);
        }

        if (requested_topics)
                rd_kafka_topic_partition_list_destroy(requested_topics);

        rd_kafka_mock_connection_send_response(mconn, rkbuf, resp);

        return 0;

 err_parse:
        if (requested_topics)
                rd_kafka_topic_partition_list_destroy(requested_topics);

        rd_kafka_buf_destroy(resp);
        return -1;

}


/**
 * @brief Handle FindCoordinatorRequest
 */
static int
rd_kafka_mock_handle_FindCoordinator (rd_kafka_mock_connection_t *mconn,
                                      rd_kafka_buf_t *rkbuf) {
        rd_kafka_mock_cluster_t *mcluster = mconn->broker->cluster;
        const rd_bool_t log_decode_errors = rd_true;
        rd_kafka_buf_t *resp = rd_kafka_mock_buf_new_response(rkbuf);
        rd_kafkap_str_t Key;
        int8_t KeyType = RD_KAFKA_COORD_GROUP;
        const rd_kafka_mock_broker_t *mrkb = NULL;
        rd_kafka_resp_err_t err;

        /* Key */
        rd_kafka_buf_read_str(rkbuf, &Key);

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 1) {
                /* KeyType */
                rd_kafka_buf_read_i8(rkbuf, &KeyType);
        }


        /*
         * Construct response
         */
        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 1) {
                /* Response: Throttle */
                rd_kafka_buf_write_i32(resp, 0);
        }

        /* Inject error, if any */
        err = rd_kafka_mock_next_request_error(mcluster,
                                               rkbuf->rkbuf_reqhdr.ApiKey);

        if (!err && RD_KAFKAP_STR_LEN(&Key) > 0) {
                mrkb = rd_kafka_mock_cluster_get_coord(mcluster,
                                                       KeyType, &Key);
                rd_assert(mrkb);
        }

        if (!mrkb && !err)
                err = RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE;

        if (err) {
                /* Response: ErrorCode and ErrorMessage */
                rd_kafka_buf_write_i16(resp, err);
                if (rkbuf->rkbuf_reqhdr.ApiVersion >= 1)
                        rd_kafka_buf_write_str(resp, rd_kafka_err2str(err), -1);

                /* Response: NodeId, Host, Port */
                rd_kafka_buf_write_i32(resp, -1);
                rd_kafka_buf_write_str(resp, NULL, -1);
                rd_kafka_buf_write_i32(resp, -1);
        } else {
                /* Response: ErrorCode and ErrorMessage */
                rd_kafka_buf_write_i16(resp, 0);
                if (rkbuf->rkbuf_reqhdr.ApiVersion >= 1)
                        rd_kafka_buf_write_str(resp, NULL, -1);

                /* Response: NodeId, Host, Port */
                rd_kafka_buf_write_i32(resp, mrkb->id);
                rd_kafka_buf_write_str(resp, mrkb->advertised_listener, -1);
                rd_kafka_buf_write_i32(resp, mrkb->port);
        }

        rd_kafka_mock_connection_send_response(mconn, rkbuf, resp);
        return 0;

 err_parse:
        rd_kafka_buf_destroy(resp);
        return -1;
}



/**
 * @brief Generate a unique ProducerID
 */
static void rd_kafka_mock_pid_generate (rd_kafka_mock_cluster_t *mcluster,
                                        rd_kafka_pid_t *pid) {
        pid->id = rd_jitter(1, 900000) * 1000;
        pid->epoch = 0;
}


/**
 * @brief Handle InitProducerId
 */
static int
rd_kafka_mock_handle_InitProducerId (rd_kafka_mock_connection_t *mconn,
                                     rd_kafka_buf_t *rkbuf) {
        rd_kafka_mock_cluster_t *mcluster = mconn->broker->cluster;
        const rd_bool_t log_decode_errors = rd_true;
        rd_kafka_buf_t *resp = rd_kafka_mock_buf_new_response(rkbuf);
        rd_kafkap_str_t TransactionalId;
        rd_kafka_pid_t pid;
        int32_t TxnTimeoutMs;
        rd_kafka_resp_err_t err;

        /* TransactionalId */
        rd_kafka_buf_read_str(rkbuf, &TransactionalId);
        /* TransactionTimeoutMs */
        rd_kafka_buf_read_i32(rkbuf, &TxnTimeoutMs);

        /*
         * Construct response
         */

        /* ThrottleTimeMs */
        rd_kafka_buf_write_i32(resp, 0);

        /* Inject error */
        err = rd_kafka_mock_next_request_error(mcluster,
                                               rkbuf->rkbuf_reqhdr.ApiKey);

        if (!err &&
            rd_kafka_mock_cluster_get_coord(mcluster,
                                            RD_KAFKA_COORD_TXN,
                                            &TransactionalId) != mconn->broker)
                err = RD_KAFKA_RESP_ERR_NOT_COORDINATOR;

        /* ErrorCode */
        rd_kafka_buf_write_i16(resp, err);

        if (!err)
                rd_kafka_mock_pid_generate(mcluster, &pid);
        else
                rd_kafka_pid_reset(&pid);

        /* ProducerId */
        rd_kafka_buf_write_i64(resp, pid.id);
        /* ProducerEpoch */
        rd_kafka_buf_write_i16(resp, pid.epoch);

        rd_kafka_mock_connection_send_response(mconn, rkbuf, resp);

        return 0;

 err_parse:
        rd_kafka_buf_destroy(resp);
        return -1;
}



/**
 * @brief Handle AddPartitionsToTxn
 */
static int
rd_kafka_mock_handle_AddPartitionsToTxn (rd_kafka_mock_connection_t *mconn,
                                         rd_kafka_buf_t *rkbuf) {
        rd_kafka_mock_cluster_t *mcluster = mconn->broker->cluster;
        const rd_bool_t log_decode_errors = rd_true;
        rd_kafka_buf_t *resp = rd_kafka_mock_buf_new_response(rkbuf);
        rd_kafka_resp_err_t all_err;
        rd_kafkap_str_t TransactionalId;
        rd_kafka_pid_t pid;
        int32_t TopicsCnt;

        /* Response: ThrottleTimeMs */
        rd_kafka_buf_write_i32(resp, 0);

        /* TransactionalId */
        rd_kafka_buf_read_str(rkbuf, &TransactionalId);
        /* ProducerId */
        rd_kafka_buf_read_i64(rkbuf, &pid.id);
        /* Epoch */
        rd_kafka_buf_read_i16(rkbuf, &pid.epoch);
        /* #Topics */
        rd_kafka_buf_read_i32(rkbuf, &TopicsCnt);

        /* Response: #Results */
        rd_kafka_buf_write_i32(resp, TopicsCnt);

        /* Inject error */
        all_err = rd_kafka_mock_next_request_error(mcluster,
                                                   rkbuf->rkbuf_reqhdr.ApiKey);

        if (!all_err &&
            rd_kafka_mock_cluster_get_coord(mcluster,
                                            RD_KAFKA_COORD_TXN,
                                            &TransactionalId) != mconn->broker)
                all_err = RD_KAFKA_RESP_ERR_NOT_COORDINATOR;

        while (TopicsCnt-- > 0) {
                rd_kafkap_str_t Topic;
                int32_t PartsCnt;
                const rd_kafka_mock_topic_t *mtopic;

                /* Topic */
                rd_kafka_buf_read_str(rkbuf, &Topic);
                /* Response: Topic */
                rd_kafka_buf_write_kstr(resp, &Topic);

                /* #Partitions */
                rd_kafka_buf_read_i32(rkbuf, &PartsCnt);
                /* Response: #Partitions */
                rd_kafka_buf_write_i32(resp, PartsCnt);

                mtopic = rd_kafka_mock_topic_find_by_kstr(mcluster, &Topic);

                while (PartsCnt--) {
                        int32_t Partition;
                        rd_kafka_resp_err_t err = all_err;

                        /* Partition */
                        rd_kafka_buf_read_i32(rkbuf, &Partition);
                        /* Response: Partition */
                        rd_kafka_buf_write_i32(resp, Partition);

                        if (!mtopic ||
                            Partition < 0 || Partition >= mtopic->partition_cnt)
                                err = RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART;
                        else if (mtopic && mtopic->err)
                                err = mtopic->err;

                        /* Response: ErrorCode */
                        rd_kafka_buf_write_i16(resp, err);
                }
        }

        rd_kafka_mock_connection_send_response(mconn, rkbuf, resp);

        return 0;

 err_parse:
        rd_kafka_buf_destroy(resp);
        return -1;
}


/**
 * @brief Handle AddOffsetsToTxn
 */
static int
rd_kafka_mock_handle_AddOffsetsToTxn (rd_kafka_mock_connection_t *mconn,
                                         rd_kafka_buf_t *rkbuf) {
        rd_kafka_mock_cluster_t *mcluster = mconn->broker->cluster;
        const rd_bool_t log_decode_errors = rd_true;
        rd_kafka_buf_t *resp = rd_kafka_mock_buf_new_response(rkbuf);
        rd_kafka_resp_err_t err;
        rd_kafkap_str_t TransactionalId, GroupId;
        rd_kafka_pid_t pid;

        /* TransactionalId */
        rd_kafka_buf_read_str(rkbuf, &TransactionalId);
        /* ProducerId */
        rd_kafka_buf_read_i64(rkbuf, &pid.id);
        /* Epoch */
        rd_kafka_buf_read_i16(rkbuf, &pid.epoch);
        /* GroupIdId */
        rd_kafka_buf_read_str(rkbuf, &GroupId);

        /* Response: ThrottleTimeMs */
        rd_kafka_buf_write_i32(resp, 0);

        /* Inject error */
        err = rd_kafka_mock_next_request_error(mcluster,
                                               rkbuf->rkbuf_reqhdr.ApiKey);

        if (!err &&
            rd_kafka_mock_cluster_get_coord(mcluster,
                                            RD_KAFKA_COORD_TXN,
                                            &TransactionalId) != mconn->broker)
                err = RD_KAFKA_RESP_ERR_NOT_COORDINATOR;

        /* Response: ErrorCode */
        rd_kafka_buf_write_i16(resp, err);

        rd_kafka_mock_connection_send_response(mconn, rkbuf, resp);

        return 0;

 err_parse:
        rd_kafka_buf_destroy(resp);
        return -1;
}


/**
 * @brief Handle TxnOffsetCommit
 */
static int
rd_kafka_mock_handle_TxnOffsetCommit (rd_kafka_mock_connection_t *mconn,
                                      rd_kafka_buf_t *rkbuf) {
        rd_kafka_mock_cluster_t *mcluster = mconn->broker->cluster;
        const rd_bool_t log_decode_errors = rd_true;
        rd_kafka_buf_t *resp = rd_kafka_mock_buf_new_response(rkbuf);
        rd_kafka_resp_err_t err;
        rd_kafkap_str_t TransactionalId, GroupId;
        rd_kafka_pid_t pid;
        int32_t TopicsCnt;

        /* Response: ThrottleTimeMs */
        rd_kafka_buf_write_i32(resp, 0);

        /* TransactionalId */
        rd_kafka_buf_read_str(rkbuf, &TransactionalId);
        /* GroupId */
        rd_kafka_buf_read_str(rkbuf, &GroupId);
        /* ProducerId */
        rd_kafka_buf_read_i64(rkbuf, &pid.id);
        /* Epoch */
        rd_kafka_buf_read_i16(rkbuf, &pid.epoch);
        /* #Topics */
        rd_kafka_buf_read_i32(rkbuf, &TopicsCnt);

        /* Response: #Results */
        rd_kafka_buf_write_i32(resp, TopicsCnt);

        /* Inject error */
        err = rd_kafka_mock_next_request_error(mcluster,
                                               rkbuf->rkbuf_reqhdr.ApiKey);

        if (!err &&
            rd_kafka_mock_cluster_get_coord(mcluster,
                                            RD_KAFKA_COORD_TXN,
                                            &TransactionalId) != mconn->broker)
                err = RD_KAFKA_RESP_ERR_NOT_COORDINATOR;

        while (TopicsCnt-- > 0) {
                rd_kafkap_str_t Topic;
                int32_t PartsCnt;

                /* Topic */
                rd_kafka_buf_read_str(rkbuf, &Topic);
                /* Response: Topic */
                rd_kafka_buf_write_kstr(resp, &Topic);

                /* #Partitions */
                rd_kafka_buf_read_i32(rkbuf, &PartsCnt);
                /* Response: #Partitions */
                rd_kafka_buf_write_i32(resp, PartsCnt);

                /* Ignore if the topic or partition exists or not. */

                while (PartsCnt-- > 0) {
                        int32_t Partition;
                        int64_t Offset;
                        rd_kafkap_str_t Metadata;

                        /* Partition */
                        rd_kafka_buf_read_i32(rkbuf, &Partition);
                        /* Response: Partition */
                        rd_kafka_buf_write_i32(resp, Partition);

                        /* CommittedOffset */
                        rd_kafka_buf_read_i64(rkbuf, &Offset);

                        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 2) {
                                /* CommittedLeaderEpoch */
                                int32_t Epoch;
                                rd_kafka_buf_read_i32(rkbuf, &Epoch);
                        }

                        /* CommittedMetadata */
                        rd_kafka_buf_read_str(rkbuf, &Metadata);

                        /* Response: ErrorCode */
                        rd_kafka_buf_write_i16(resp, err);
                }
        }

        rd_kafka_mock_connection_send_response(mconn, rkbuf, resp);

        return 0;

 err_parse:
        rd_kafka_buf_destroy(resp);
        return -1;
}


/**
 * @brief Handle EndTxn
 */
static int
rd_kafka_mock_handle_EndTxn (rd_kafka_mock_connection_t *mconn,
                             rd_kafka_buf_t *rkbuf) {
        rd_kafka_mock_cluster_t *mcluster = mconn->broker->cluster;
        const rd_bool_t log_decode_errors = rd_true;
        rd_kafka_buf_t *resp = rd_kafka_mock_buf_new_response(rkbuf);
        rd_kafka_resp_err_t err;
        rd_kafkap_str_t TransactionalId;
        rd_kafka_pid_t pid;
        rd_bool_t committed;

        /* TransactionalId */
        rd_kafka_buf_read_str(rkbuf, &TransactionalId);
        /* ProducerId */
        rd_kafka_buf_read_i64(rkbuf, &pid.id);
        /* ProducerEpoch */
        rd_kafka_buf_read_i16(rkbuf, &pid.epoch);
        /* Committed */
        rd_kafka_buf_read_bool(rkbuf, &committed);

        /*
         * Construct response
         */

        /* ThrottleTimeMs */
        rd_kafka_buf_write_i32(resp, 0);

        /* Inject error */
        err = rd_kafka_mock_next_request_error(mcluster,
                                               rkbuf->rkbuf_reqhdr.ApiKey);

        if (!err &&
            rd_kafka_mock_cluster_get_coord(mcluster,
                                            RD_KAFKA_COORD_TXN,
                                            &TransactionalId) != mconn->broker)
                err = RD_KAFKA_RESP_ERR_NOT_COORDINATOR;

        /* ErrorCode */
        rd_kafka_buf_write_i16(resp, err);

        rd_kafka_mock_connection_send_response(mconn, rkbuf, resp);

        return 0;

 err_parse:
        rd_kafka_buf_destroy(resp);
        return -1;
}


/**
 * @brief Default request handlers
 */
const struct rd_kafka_mock_api_handler
rd_kafka_mock_api_handlers[RD_KAFKAP__NUM] = {
        [RD_KAFKAP_Produce] = { 0, 7, rd_kafka_mock_handle_Produce },
        [RD_KAFKAP_Fetch] = { 0, 11, rd_kafka_mock_handle_Fetch },
        [RD_KAFKAP_Offset] = { 0, 5, rd_kafka_mock_handle_ListOffset },
        [RD_KAFKAP_OffsetFetch] = { 0, 5, rd_kafka_mock_handle_OffsetFetch },
        [RD_KAFKAP_OffsetCommit] = { 0, 7, rd_kafka_mock_handle_OffsetCommit },
        [RD_KAFKAP_ApiVersion] = { 0, 2, rd_kafka_mock_handle_ApiVersion },
        [RD_KAFKAP_Metadata] = { 0, 2, rd_kafka_mock_handle_Metadata },
        [RD_KAFKAP_FindCoordinator] = { 0, 2,
                                        rd_kafka_mock_handle_FindCoordinator },
        [RD_KAFKAP_InitProducerId] = { 0, 1,
                                       rd_kafka_mock_handle_InitProducerId },
        [RD_KAFKAP_AddPartitionsToTxn]  = { 0, 1,
                                            rd_kafka_mock_handle_AddPartitionsToTxn },
        [RD_KAFKAP_AddOffsetsToTxn] = { 0, 1,
                                        rd_kafka_mock_handle_AddOffsetsToTxn },
        [RD_KAFKAP_TxnOffsetCommit] = { 0, 2,
                                        rd_kafka_mock_handle_TxnOffsetCommit },
        [RD_KAFKAP_EndTxn] = { 0, 1, rd_kafka_mock_handle_EndTxn },
};



/**
 * @brief Handle ApiVersionRequest
 */
static int rd_kafka_mock_handle_ApiVersion (rd_kafka_mock_connection_t *mconn,
                                            rd_kafka_buf_t *rkbuf) {
        rd_kafka_buf_t *resp = rd_kafka_mock_buf_new_response(rkbuf);
        size_t of_ApiKeysCnt;
        int cnt = 0;
        int i;

        /* ErrorCode */
        rd_kafka_buf_write_i16(resp, RD_KAFKA_RESP_ERR_NO_ERROR);

        /* #ApiKeys */
        of_ApiKeysCnt = rd_kafka_buf_write_i32(resp, 0); /* updated later */

        for (i = 0 ; i < RD_KAFKAP__NUM ; i++) {
                if (!rd_kafka_mock_api_handlers[i].cb)
                        continue;

                /* ApiKey */
                rd_kafka_buf_write_i16(resp, (int16_t)i);
                /* MinVersion */
                rd_kafka_buf_write_i16(
                        resp, rd_kafka_mock_api_handlers[i].MinVersion);
                /* MaxVersion */
                rd_kafka_buf_write_i16(
                        resp, rd_kafka_mock_api_handlers[i].MaxVersion);

                cnt++;
        }

        rd_kafka_buf_update_i32(resp, of_ApiKeysCnt, cnt);

        if (rkbuf->rkbuf_reqhdr.ApiVersion >= 1) {
                /* ThrottletimeMs */
                rd_kafka_buf_write_i32(resp, 0);
        }

        rd_kafka_mock_connection_send_response(mconn, rkbuf, resp);

        return 0;
}
