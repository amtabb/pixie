#include "src/stirling/socket_trace_connector.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sys/socket.h>
#include <memory>

#include "src/stirling/bcc_bpf/socket_trace.h"

namespace pl {
namespace stirling {

using ::testing::ElementsAre;
using RecordBatch = types::ColumnWrapperRecordBatch;

class SocketTraceConnectorTest : public ::testing::Test {
 protected:
  static constexpr uint32_t kPID = 12345;
  static constexpr uint32_t kFD = 3;

  void SetUp() override {
    // Create and configure the connector.
    connector_ = SocketTraceConnector::Create("socket_trace_connector");
    source_ = dynamic_cast<SocketTraceConnector*>(connector_.get());
    ASSERT_NE(nullptr, source_);
    source_->TestOnlyConfigure(kProtocolHTTP, kSocketTraceSendReq | kSocketTraceRecvResp);

    // Because some tests change the inactivity duration, make sure to reset it here for each test.
    ConnectionTracker::SetDefaultInactivityDuration();
  }

  conn_info_t InitConn(uint64_t ts_ns = 0) {
    ++generation_;

    conn_info_t conn_info{};
    conn_info.addr.sin6_family = AF_INET;
    conn_info.timestamp_ns = ts_ns;
    conn_info.conn_id.pid = kPID;
    conn_info.conn_id.fd = kFD;
    conn_info.conn_id.generation = generation_;
    conn_info.traffic_class.protocol = kProtocolHTTP;
    conn_info.traffic_class.role = kRoleRequestor;
    conn_info.rd_seq_num = 0;
    conn_info.wr_seq_num = 0;
    return conn_info;
  }

  SocketDataEvent InitSendEvent(std::string_view msg, uint64_t ts_ns = 0) {
    SocketDataEvent event = InitDataEvent(kEventTypeSyscallSendEvent, msg, ts_ns);
    event.attr.seq_num = send_seq_num_;
    send_seq_num_++;
    return event;
  }

  SocketDataEvent InitRecvEvent(std::string_view msg, uint64_t ts_ns = 0) {
    SocketDataEvent event = InitDataEvent(kEventTypeSyscallRecvEvent, msg, ts_ns);
    event.attr.seq_num = recv_seq_num_;
    recv_seq_num_++;
    return event;
  }

  SocketDataEvent InitDataEvent(EventType event_type, std::string_view msg, uint64_t ts_ns = 0) {
    socket_data_event_t event = {};
    event.attr.event_type = event_type;
    event.attr.traffic_class.protocol = kProtocolHTTP;
    event.attr.traffic_class.role = kRoleRequestor;
    event.attr.timestamp_ns = ts_ns;
    event.attr.conn_id.pid = kPID;
    event.attr.conn_id.fd = kFD;
    event.attr.conn_id.generation = generation_;
    event.attr.msg_size = msg.size();
    msg.copy(event.msg, msg.size());
    return SocketDataEvent(&event);
  }

  conn_info_t InitClose() {
    conn_info_t conn_info{};
    conn_info.timestamp_ns = 1;
    conn_info.conn_id.pid = kPID;
    conn_info.conn_id.fd = kFD;
    conn_info.conn_id.generation = generation_;
    conn_info.rd_seq_num = recv_seq_num_;
    conn_info.wr_seq_num = send_seq_num_;
    return conn_info;
  }

  types::ColumnWrapperRecordBatch GetRecordBatch(DataTableSchema schema) {
    types::ColumnWrapperRecordBatch record_batch;
    InitRecordBatch(schema.elements(), /*target_capacity*/ 1, &record_batch);
    return record_batch;
  }

  uint32_t generation_ = 0;
  uint64_t send_seq_num_ = 0;
  uint64_t recv_seq_num_ = 0;

  static constexpr int kTableNum = SocketTraceConnector::kHTTPTableNum;
  static constexpr DataTableSchema kHTTPTable = SocketTraceConnector::kHTTPTable;
  static constexpr int kHTTPRespBodyIdx = kHTTPTable.ColIndex("http_resp_body");
  static constexpr int kHTTPReqMethodIdx = kHTTPTable.ColIndex("http_req_method");
  static constexpr int kHTTPReqPathIdx = kHTTPTable.ColIndex("http_req_path");
  static constexpr int kTimeIdx = kHTTPTable.ColIndex("time_");

  std::unique_ptr<SourceConnector> connector_;
  SocketTraceConnector* source_ = nullptr;

  const std::string kReq0 =
      "GET /index.html HTTP/1.1\r\n"
      "Host: www.pixielabs.ai\r\n"
      "User-Agent: Mozilla/5.0 (X11; Linux x86_64)\r\n"
      "\r\n";

  const std::string kReq1 =
      "GET /data.html HTTP/1.1\r\n"
      "Host: www.pixielabs.ai\r\n"
      "User-Agent: Mozilla/5.0 (X11; Linux x86_64)\r\n"
      "\r\n";

  const std::string kReq2 =
      "GET /logs.html HTTP/1.1\r\n"
      "Host: www.pixielabs.ai\r\n"
      "User-Agent: Mozilla/5.0 (X11; Linux x86_64)\r\n"
      "\r\n";

  const std::string kJSONResp =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: application/json; charset=utf-8\r\n"
      "Content-Length: 3\r\n"
      "\r\n"
      "foo";

  const std::string kTextResp =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "Content-Length: 3\r\n"
      "\r\n"
      "bar";

  const std::string_view kResp0 =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: json\r\n"
      "Content-Length: 3\r\n"
      "\r\n"
      "foo";

  const std::string_view kResp1 =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: json\r\n"
      "Content-Length: 3\r\n"
      "\r\n"
      "bar";

  std::string_view kResp2 =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: json\r\n"
      "Content-Length: 3\r\n"
      "\r\n"
      "doe";
};

auto ToStringVector(const types::SharedColumnWrapper& col) {
  std::vector<std::string> result;
  for (size_t i = 0; i < col->Size(); ++i) {
    result.push_back(col->Get<types::StringValue>(i));
  }
  return result;
}

template <class TValueType>
auto ToIntVector(const types::SharedColumnWrapper& col) {
  std::vector<int64_t> result;
  for (size_t i = 0; i < col->Size(); ++i) {
    result.push_back(col->Get<TValueType>(i).val);
  }
  return result;
}

TEST_F(SocketTraceConnectorTest, End2end) {
  conn_info_t conn = InitConn(50);
  SocketDataEvent event0_json = InitRecvEvent(kJSONResp, 100);
  SocketDataEvent event1_text = InitRecvEvent(kTextResp, 200);
  SocketDataEvent event2_text = InitRecvEvent(kTextResp, 200);
  SocketDataEvent event3_json = InitRecvEvent(kJSONResp, 100);
  conn_info_t close_conn = InitClose();

  auto record_batch = GetRecordBatch(SocketTraceConnector::kHTTPTable);

  source_->InitClockRealTimeOffset();
  EXPECT_NE(0, source_->ClockRealTimeOffset());

  // Registers a new connection
  source_->AcceptOpenConnEvent(conn);

  ASSERT_THAT(source_->NumActiveConnections(), 1);

  conn_id_t search_conn_id;
  search_conn_id.pid = kPID;
  search_conn_id.fd = kFD;
  search_conn_id.generation = 1;
  const ConnectionTracker* tracker = source_->GetConnectionTracker(search_conn_id);
  ASSERT_NE(nullptr, tracker);
  EXPECT_EQ(50 + source_->ClockRealTimeOffset(), tracker->conn().timestamp_ns);

  // AcceptDataEvent() puts data into the internal buffer of SocketTraceConnector. And then
  // TransferData() polls perf buffer, which is no-op because we did not initialize probes, and the
  // data in the internal buffer is being processed and filtered.
  source_->AcceptDataEvent(event0_json);
  source_->TransferData(kTableNum, &record_batch);
  for (const auto& column : record_batch) {
    EXPECT_EQ(1, column->Size())
        << "event_json Content-Type does have 'json', and will be selected by the default filter";
  }

  source_->AcceptDataEvent(event1_text);
  source_->TransferData(kTableNum, &record_batch);
  for (const auto& column : record_batch) {
    EXPECT_EQ(1, column->Size())
        << "event_text Content-Type has no 'json', and won't be selected by the default filter";
  }

  SocketTraceConnector::TestOnlySetHTTPResponseHeaderFilter({
      {{"Content-Type", "text/plain"}},
      {{"Content-Encoding", "gzip"}},
  });
  source_->AcceptDataEvent(event2_text);
  source_->TransferData(kTableNum, &record_batch);
  for (const auto& column : record_batch) {
    EXPECT_EQ(2, column->Size())
        << "The filter is changed to require 'text/plain' in Content-Type header, "
           "and event_json Content-Type does not match, and won't be selected";
  }

  SocketTraceConnector::TestOnlySetHTTPResponseHeaderFilter({
      {{"Content-Type", "application/json"}},
      {{"Content-Encoding", "gzip"}},
  });
  source_->AcceptDataEvent(event3_json);
  source_->AcceptCloseConnEvent(close_conn);
  source_->TransferData(kTableNum, &record_batch);
  for (const auto& column : record_batch) {
    EXPECT_EQ(3, column->Size())
        << "The filter is changed to require 'application/json' in Content-Type header, "
           "and event_json Content-Type matches, and is selected";
  }
  EXPECT_THAT(ToStringVector(record_batch[kHTTPRespBodyIdx]), ElementsAre("foo", "bar", "foo"));
  EXPECT_THAT(
      ToIntVector<types::Time64NSValue>(record_batch[kTimeIdx]),
      ElementsAre(100 + source_->ClockRealTimeOffset(), 200 + source_->ClockRealTimeOffset(),
                  100 + source_->ClockRealTimeOffset()));
}

TEST_F(SocketTraceConnectorTest, AppendNonContiguousEvents) {
  conn_info_t conn = InitConn();
  SocketDataEvent event0 =
      InitRecvEvent(absl::StrCat(kResp0, kResp1.substr(0, kResp1.length() / 2)));
  SocketDataEvent event1 = InitRecvEvent(kResp1.substr(kResp1.length() / 2));
  SocketDataEvent event2 = InitRecvEvent(kResp2);
  conn_info_t close_conn = InitClose();

  auto record_batch = GetRecordBatch(SocketTraceConnector::kHTTPTable);

  source_->AcceptOpenConnEvent(conn);
  source_->AcceptDataEvent(event0);
  source_->AcceptDataEvent(event2);
  source_->TransferData(kTableNum, &record_batch);
  EXPECT_EQ(1, record_batch[0]->Size());

  source_->AcceptDataEvent(event1);
  source_->AcceptCloseConnEvent(close_conn);
  source_->TransferData(kTableNum, &record_batch);
  EXPECT_EQ(3, record_batch[0]->Size()) << "Get 3 events after getting the missing one.";
}

TEST_F(SocketTraceConnectorTest, NoEvents) {
  conn_info_t conn = InitConn();
  SocketDataEvent event0 = InitRecvEvent(kResp0);
  conn_info_t close_conn = InitClose();

  auto record_batch = GetRecordBatch(SocketTraceConnector::kHTTPTable);

  source_->AcceptOpenConnEvent(conn);

  // Check empty transfer.
  source_->TransferData(kTableNum, &record_batch);
  EXPECT_EQ(0, record_batch[0]->Size());

  // Check empty transfer following a successful transfer.
  source_->AcceptDataEvent(event0);
  source_->TransferData(kTableNum, &record_batch);
  EXPECT_EQ(1, record_batch[0]->Size());
  source_->TransferData(kTableNum, &record_batch);
  EXPECT_EQ(1, record_batch[0]->Size());

  EXPECT_EQ(1, source_->NumActiveConnections());
  source_->AcceptCloseConnEvent(close_conn);
  source_->TransferData(kTableNum, &record_batch);
}

TEST_F(SocketTraceConnectorTest, RequestResponseMatching) {
  conn_info_t conn = InitConn();
  SocketDataEvent req_event0 = InitSendEvent(kReq0);
  SocketDataEvent req_event1 = InitSendEvent(kReq1);
  SocketDataEvent req_event2 = InitSendEvent(kReq2);
  SocketDataEvent resp_event0 = InitRecvEvent(kResp0);
  SocketDataEvent resp_event1 = InitRecvEvent(kResp1);
  SocketDataEvent resp_event2 = InitRecvEvent(kResp2);
  conn_info_t close_conn = InitClose();

  auto record_batch = GetRecordBatch(SocketTraceConnector::kHTTPTable);

  source_->AcceptOpenConnEvent(conn);
  source_->AcceptDataEvent(req_event0);
  source_->AcceptDataEvent(req_event1);
  source_->AcceptDataEvent(req_event2);
  source_->AcceptDataEvent(resp_event0);
  source_->AcceptDataEvent(resp_event1);
  source_->AcceptDataEvent(resp_event2);
  source_->AcceptCloseConnEvent(close_conn);
  source_->TransferData(kTableNum, &record_batch);
  EXPECT_EQ(3, record_batch[0]->Size());

  EXPECT_THAT(ToStringVector(record_batch[kHTTPRespBodyIdx]), ElementsAre("foo", "bar", "doe"));
  EXPECT_THAT(ToStringVector(record_batch[kHTTPReqMethodIdx]), ElementsAre("GET", "GET", "GET"));
  EXPECT_THAT(ToStringVector(record_batch[kHTTPReqPathIdx]),
              ElementsAre("/index.html", "/data.html", "/logs.html"));
}

TEST_F(SocketTraceConnectorTest, ConnectionCleanupInOrder) {
  conn_info_t conn = InitConn();
  SocketDataEvent req_event0 = InitSendEvent(kReq0);
  SocketDataEvent req_event1 = InitSendEvent(kReq1);
  SocketDataEvent req_event2 = InitSendEvent(kReq2);
  SocketDataEvent resp_event0 = InitRecvEvent(kResp0);
  SocketDataEvent resp_event1 = InitRecvEvent(kResp1);
  SocketDataEvent resp_event2 = InitRecvEvent(kResp2);
  conn_info_t close_conn = InitClose();

  auto record_batch = GetRecordBatch(SocketTraceConnector::kHTTPTable);

  EXPECT_EQ(0, source_->NumActiveConnections());

  source_->AcceptOpenConnEvent(conn);

  EXPECT_EQ(1, source_->NumActiveConnections());
  source_->TransferData(kTableNum, &record_batch);
  EXPECT_EQ(1, source_->NumActiveConnections());

  source_->AcceptDataEvent(req_event0);
  source_->AcceptDataEvent(req_event2);
  source_->AcceptDataEvent(req_event1);
  source_->AcceptDataEvent(resp_event0);
  source_->AcceptDataEvent(resp_event1);
  source_->AcceptDataEvent(resp_event2);

  EXPECT_EQ(1, source_->NumActiveConnections());
  source_->TransferData(kTableNum, &record_batch);
  EXPECT_EQ(1, source_->NumActiveConnections());

  source_->AcceptCloseConnEvent(close_conn);
  // CloseConnEvent results in countdown = kDeathCountdownIters.

  // Death countdown period: keep calling Transfer Data to increment iterations.
  for (int32_t i = 0; i < ConnectionTracker::kDeathCountdownIters - 1; ++i) {
    EXPECT_EQ(1, source_->NumActiveConnections());
    source_->TransferData(kTableNum, &record_batch);
  }

  EXPECT_EQ(1, source_->NumActiveConnections());
  source_->TransferData(kTableNum, &record_batch);
  EXPECT_EQ(0, source_->NumActiveConnections());
}

TEST_F(SocketTraceConnectorTest, ConnectionCleanupOutOfOrder) {
  conn_info_t conn = InitConn();
  SocketDataEvent req_event0 = InitSendEvent(kReq0);
  SocketDataEvent req_event1 = InitSendEvent(kReq1);
  SocketDataEvent req_event2 = InitSendEvent(kReq2);
  SocketDataEvent resp_event0 = InitRecvEvent(kResp0);
  SocketDataEvent resp_event1 = InitRecvEvent(kResp1);
  SocketDataEvent resp_event2 = InitRecvEvent(kResp2);
  conn_info_t close_conn = InitClose();

  auto record_batch = GetRecordBatch(SocketTraceConnector::kHTTPTable);

  source_->AcceptDataEvent(req_event1);
  source_->AcceptOpenConnEvent(conn);
  source_->AcceptDataEvent(req_event0);
  source_->AcceptDataEvent(resp_event2);
  source_->AcceptDataEvent(resp_event0);

  source_->TransferData(kTableNum, &record_batch);
  EXPECT_EQ(1, source_->NumActiveConnections());

  source_->AcceptCloseConnEvent(close_conn);
  source_->AcceptDataEvent(resp_event1);
  source_->AcceptDataEvent(req_event2);

  // CloseConnEvent results in countdown = kDeathCountdownIters.

  // Death countdown period: keep calling Transfer Data to increment iterations.
  for (int32_t i = 0; i < ConnectionTracker::kDeathCountdownIters - 1; ++i) {
    source_->TransferData(kTableNum, &record_batch);
    EXPECT_EQ(1, source_->NumActiveConnections());
  }

  source_->TransferData(kTableNum, &record_batch);
  EXPECT_EQ(0, source_->NumActiveConnections());
}

TEST_F(SocketTraceConnectorTest, ConnectionCleanupMissingDataEvent) {
  conn_info_t conn = InitConn();
  SocketDataEvent req_event0 = InitSendEvent(kReq0);
  SocketDataEvent req_event1 = InitSendEvent(kReq1);
  SocketDataEvent req_event2 = InitSendEvent(kReq2);
  SocketDataEvent resp_event0 = InitRecvEvent(kResp0);
  SocketDataEvent resp_event1 = InitRecvEvent(kResp1);
  SocketDataEvent resp_event2 = InitRecvEvent(kResp2);
  conn_info_t close_conn = InitClose();

  auto record_batch = GetRecordBatch(SocketTraceConnector::kHTTPTable);

  source_->AcceptOpenConnEvent(conn);
  source_->AcceptDataEvent(req_event0);
  source_->AcceptDataEvent(req_event1);
  source_->AcceptDataEvent(req_event2);
  source_->AcceptDataEvent(resp_event0);
  // Missing event: source_->AcceptDataEvent(resp_event1);
  source_->AcceptDataEvent(resp_event2);
  source_->AcceptCloseConnEvent(close_conn);

  // CloseConnEvent results in countdown = kDeathCountdownIters.

  // Death countdown period: keep calling Transfer Data to increment iterations.
  for (int32_t i = 0; i < ConnectionTracker::kDeathCountdownIters - 1; ++i) {
    source_->TransferData(kTableNum, &record_batch);
    EXPECT_EQ(1, source_->NumActiveConnections());
  }

  source_->TransferData(kTableNum, &record_batch);
  EXPECT_EQ(0, source_->NumActiveConnections());
}

TEST_F(SocketTraceConnectorTest, ConnectionCleanupOldGenerations) {
  conn_info_t conn0 = InitConn();
  SocketDataEvent conn0_req_event = InitSendEvent(kReq0);
  SocketDataEvent conn0_resp_event = InitRecvEvent(kResp0);
  conn_info_t conn0_close = InitClose();

  conn_info_t conn1 = InitConn();
  SocketDataEvent conn1_req_event = InitSendEvent(kReq1);
  SocketDataEvent conn1_resp_event = InitRecvEvent(kResp1);
  conn_info_t conn1_close = InitClose();

  conn_info_t conn2 = InitConn();
  SocketDataEvent conn2_req_event = InitSendEvent(kReq2);
  SocketDataEvent conn2_resp_event = InitRecvEvent(kResp2);
  conn_info_t conn2_close = InitClose();

  auto record_batch = GetRecordBatch(SocketTraceConnector::kHTTPTable);

  // Simulating scrambled order due to perf buffer, with a couple missing events.
  source_->AcceptDataEvent(conn0_req_event);
  source_->AcceptOpenConnEvent(conn1);
  source_->AcceptCloseConnEvent(conn2_close);
  source_->AcceptDataEvent(conn0_resp_event);
  source_->AcceptOpenConnEvent(conn0);
  source_->AcceptDataEvent(conn2_req_event);
  source_->AcceptDataEvent(conn1_resp_event);
  source_->AcceptDataEvent(conn1_req_event);
  source_->AcceptOpenConnEvent(conn2);
  source_->AcceptDataEvent(conn2_resp_event);
  PL_UNUSED(conn0_close);  // Missing close event.
  PL_UNUSED(conn1_close);  // Missing close event.

  source_->TransferData(kTableNum, &record_batch);
  EXPECT_EQ(1, source_->NumActiveConnections());

  // TransferData results in countdown = kDeathCountdownIters for old generations.

  // Death countdown period: keep calling Transfer Data to increment iterations.
  for (int32_t i = 0; i < ConnectionTracker::kDeathCountdownIters - 1; ++i) {
    source_->TransferData(kTableNum, &record_batch);
    EXPECT_EQ(1, source_->NumActiveConnections());
  }

  source_->TransferData(kTableNum, &record_batch);
  EXPECT_EQ(0, source_->NumActiveConnections());
}

TEST_F(SocketTraceConnectorTest, ConnectionCleanupInactiveDead) {
  ConnectionTracker::SetInactivityDuration(std::chrono::seconds(1));

  // Inactive dead connections are determined by checking the /proc filesystem.
  // Here we create a PID that is a valid number, but non-existent on any Linux system.
  // Note that max PID bits in Linux is 22 bits.
  uint32_t impossible_pid = 1 << 23;

  conn_info_t conn0 = InitConn();
  conn0.conn_id.pid = impossible_pid;

  SocketDataEvent conn0_req_event = InitSendEvent(kReq0);
  conn0_req_event.attr.conn_id.pid = impossible_pid;

  SocketDataEvent conn0_resp_event = InitRecvEvent(kResp0);
  conn0_resp_event.attr.conn_id.pid = impossible_pid;

  conn_info_t conn0_close = InitClose();
  conn0_close.conn_id.pid = impossible_pid;

  auto record_batch = GetRecordBatch(SocketTraceConnector::kHTTPTable);

  // Simulating events being emitted from BPF perf buffer.
  source_->AcceptOpenConnEvent(conn0);
  source_->AcceptDataEvent(conn0_req_event);
  source_->AcceptDataEvent(conn0_resp_event);
  PL_UNUSED(conn0_close);  // Missing close event.

  for (int i = 0; i < 100; ++i) {
    source_->TransferData(kTableNum, &record_batch);
    EXPECT_EQ(1, source_->NumActiveConnections());
  }

  sleep(2);

  // Connection should be timed out by now, and should be killed by one more TransferData() call.

  EXPECT_EQ(1, source_->NumActiveConnections());
  source_->TransferData(kTableNum, &record_batch);
  EXPECT_EQ(0, source_->NumActiveConnections());
}

TEST_F(SocketTraceConnectorTest, ConnectionCleanupInactiveAlive) {
  ConnectionTracker::SetInactivityDuration(std::chrono::seconds(1));

  // Inactive alive connections are determined by checking the /proc filesystem.
  // Here we create a PID that is a real PID, by using the test process itself.
  // And we create a real FD, by using FD 1, which is stdout.

  uint32_t real_pid = getpid();
  uint32_t real_fd = 1;

  conn_info_t conn0 = InitConn();
  conn0.conn_id.pid = real_pid;
  conn0.conn_id.fd = real_fd;

  // An incomplete message means it shouldn't be parseable (we don't want TranfserData to succeed).
  SocketDataEvent conn0_req_event = InitSendEvent("GET /index.html HTTP/1.1\r\n");
  conn0_req_event.attr.conn_id.pid = real_pid;
  conn0_req_event.attr.conn_id.fd = real_fd;

  auto record_batch = GetRecordBatch(SocketTraceConnector::kHTTPTable);

  // Simulating events being emitted from BPF perf buffer.
  source_->AcceptOpenConnEvent(conn0);
  source_->AcceptDataEvent(conn0_req_event);

  for (int i = 0; i < 100; ++i) {
    source_->TransferData(kTableNum, &record_batch);
    EXPECT_EQ(1, source_->NumActiveConnections());
  }

  conn_id_t search_conn_id;
  search_conn_id.pid = real_pid;
  search_conn_id.fd = real_fd;
  search_conn_id.generation = 1;
  const ConnectionTracker* tracker = source_->GetConnectionTracker(search_conn_id);
  ASSERT_NE(nullptr, tracker);

  // We should find some raw events in send_data.
  EXPECT_TRUE(tracker->recv_data().Empty<HTTPMessage>());
  EXPECT_FALSE(tracker->send_data().Empty<HTTPMessage>());

  sleep(2);

  // Connection should be timed out by next TransferData,
  // which should also cause events to be flushed.

  EXPECT_EQ(1, source_->NumActiveConnections());
  source_->TransferData(kTableNum, &record_batch);
  EXPECT_EQ(1, source_->NumActiveConnections());

  // Should not have transferred any data.
  EXPECT_EQ(0, record_batch[0]->Size());

  // Events should have been flushed.
  EXPECT_TRUE(tracker->recv_data().Empty<HTTPMessage>());
  EXPECT_TRUE(tracker->send_data().Empty<HTTPMessage>());
}

}  // namespace stirling
}  // namespace pl
