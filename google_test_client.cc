#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "mock_stream.h"

#include "libdialogflow_internal.h"

#include <google/cloud/dialogflow/v2beta1/session_mock.grpc.pb.h>

using google::cloud::dialogflow::v2beta1::MockSessionsStub;

using grpc::ClientReaderWriterInterface;
using google::cloud::dialogflow::v2beta1::StreamingDetectIntentRequest;
using google::cloud::dialogflow::v2beta1::StreamingDetectIntentResponse;
using google::cloud::dialogflow::v2beta1::DetectIntentRequest;
using google::cloud::dialogflow::v2beta1::DetectIntentResponse;
using google::cloud::dialogflow::v2beta1::QueryInput;
using google::cloud::dialogflow::v2beta1::EventInput;
using google::cloud::dialogflow::v2beta1::Sessions;
using grpc::Status;

using grpc::testing::MockClientReaderWriter;


using ::testing::AtLeast;
using ::testing::_;
using ::testing::AllOf;
using ::testing::Property;
using ::testing::Return;
using ::testing::DoAll;
using ::testing::SetArgPointee;

TEST(df_recognize_event, HandlesGoodInput) {
    struct dialogflow_session session;
    MockSessionsStub *stub = new MockSessionsStub();
    DetectIntentResponse response;

    response.set_response_id("12345");
    response.mutable_query_result()->set_query_text("hello");
    
    session.state = DF_STATE_READY;
    session.session = std::shared_ptr<google::cloud::dialogflow::v2beta1::Sessions::Sessions::StubInterface>(stub);

    EXPECT_CALL(*stub, DetectIntent(_, 
        Property(&DetectIntentRequest::query_input, 
            Property(&QueryInput::event,
                Property(&EventInput::name, "hello"))), _))
    .WillOnce(DoAll(SetArgPointee<2>(response), Return(Status::OK)));

    int ret = df_recognize_event(&session, "hello", NULL);

    EXPECT_EQ(ret, 0);
    EXPECT_EQ(session.results.size(), 8);

    bool foundQueryText = false;

    for (size_t i = 0; i < session.results.size(); i++) {
        if (session.results[i]->slot == "query_text") {
            EXPECT_EQ(session.results[i]->value, "hello");
            foundQueryText = true;
        }
    }

    EXPECT_TRUE(foundQueryText);
}

TEST(df_recognize_event, HandlesLanguageChange) {
    struct dialogflow_session session;
    std::shared_ptr<MockSessionsStub> stub;
    DetectIntentResponse response;

    response.set_response_id("12345");
    response.mutable_query_result()->set_query_text("hello");
    response.mutable_query_result()->set_language_code("es-MX");
    
    session.state = DF_STATE_READY;
    session.session = std::shared_ptr<google::cloud::dialogflow::v2beta1::Sessions::Sessions::StubInterface>(stub);

    EXPECT_CALL(*stub, 
        DetectIntent(_, 
            Property(&DetectIntentRequest::query_input, 
                Property(&QueryInput::event,
                    AllOf(
                        Property(&EventInput::name, "hello"),
                        Property(&EventInput::language_code, "es-MX")
                    )
                )
            ),
        _)
    )
    .WillOnce(DoAll(SetArgPointee<2>(response), Return(Status::OK)));

    int ret = df_recognize_event(&session, "hello", "es-MX");

    EXPECT_EQ(ret, 0);

    bool foundLanguage = false;

    for (size_t i = 0; i < session.results.size(); i++) {
        if (session.results[i]->slot == "language_code") {
            EXPECT_EQ(session.results[i]->value, "es-MX");
            foundLanguage = true;
        }
    }

    EXPECT_TRUE(foundLanguage);
}

#if 0
Timeboxed before i could get this working
TEST(df_start_recognition, HandlesGoodInput) {
    struct dialogflow_session session;
    std::shared_ptr<MockSessionsStub> stub;
    MockClientReaderWriter<StreamingDetectIntentRequest, StreamingDetectIntentResponse> stream;
    
    session.state = DF_STATE_READY;
    session.session = stub;

    EXPECT_CALL(*stub, StreamingDetectIntentRaw(_))
        .WillOnce(Return(stream));

    int ret = df_start_recognition(&session);

    EXPECT_EQ(ret, 0);
}
#endif

int main(int argc, char **argv) {
    ::testing::InitGoogleMock(&argc, argv);
    return RUN_ALL_TESTS(); 
}