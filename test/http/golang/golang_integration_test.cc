#include "envoy/config/bootstrap/v3/bootstrap.pb.h"
#include "envoy/extensions/filters/network/http_connection_manager/v3/http_connection_manager.pb.h"

#include "test/config/v2_link_hacks.h"
#include "test/integration/http_integration.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

using Envoy::Http::HeaderValueOf;

namespace Envoy {
namespace {

class GolangIntegrationTest : public testing::TestWithParam<Network::Address::IpVersion>,
                              public HttpIntegrationTest {
public:
  GolangIntegrationTest() : HttpIntegrationTest(Http::CodecType::HTTP1, GetParam()) {}

  void createUpstreams() override {
    HttpIntegrationTest::createUpstreams();
    addFakeUpstream(Http::CodecType::HTTP1);
    addFakeUpstream(Http::CodecType::HTTP1);
    // Create the xDS upstream.
    addFakeUpstream(Http::CodecType::HTTP2);
  }

  void initializeFilter(const std::string& filter_config, const std::string& domain = "*") {
    config_helper_.prependFilter(filter_config);

    // Create static clusters.
    createClusters();

    config_helper_.addConfigModifier(
        [domain](
            envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager&
                hcm) {
          hcm.mutable_route_config()
              ->mutable_virtual_hosts(0)
              ->mutable_routes(0)
              ->mutable_match()
              ->set_prefix("/test");

          hcm.mutable_route_config()->mutable_virtual_hosts(0)->set_domains(0, domain);
          auto* new_route = hcm.mutable_route_config()->mutable_virtual_hosts(0)->add_routes();
          new_route->mutable_match()->set_prefix("/alt/route");
          new_route->mutable_route()->set_cluster("alt_cluster");
          auto* response_header =
              new_route->mutable_response_headers_to_add()->Add()->mutable_header();
          response_header->set_key("fake_header");
          response_header->set_value("fake_value");

          const std::string key = "envoy.filters.http.golang";
          const std::string yaml =
              R"EOF(
            foo.bar:
              foo: bar
              baz: bat
            keyset:
              foo: MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAp0cSZtAdFgMI1zQJwG8ujTXFMcRY0+SA6fMZGEfQYuxcz/e8UelJ1fLDVAwYmk7KHoYzpizy0JIxAcJ+OAE+cd6a6RpwSEm/9/vizlv0vWZv2XMRAqUxk/5amlpQZE/4sRg/qJdkZZjKrSKjf5VEUQg2NytExYyYWG+3FEYpzYyUeVktmW0y/205XAuEQuxaoe+AUVKeoON1iDzvxywE42C0749XYGUFicqBSRj2eO7jm4hNWvgTapYwpswM3hV9yOAPOVQGKNXzNbLDbFTHyLw3OKayGs/4FUBa+ijlGD9VDawZq88RRaf5ztmH22gOSiKcrHXe40fsnrzh/D27uwIDAQAB
          )EOF";

          ProtobufWkt::Struct value;
          TestUtility::loadFromYaml(yaml, value);

          // Sets the route's metadata.
          hcm.mutable_route_config()
              ->mutable_virtual_hosts(0)
              ->mutable_routes(0)
              ->mutable_metadata()
              ->mutable_filter_metadata()
              ->insert(Protobuf::MapPair<std::string, ProtobufWkt::Struct>(key, value));
        });

    initialize();
  }

  void initializeSimpleFilter(const std::string& so_id) {
    addDso(so_id);

    auto yaml_fmt = R"EOF(
name: golang
typed_config:
  "@type": type.googleapis.com/envoy.extensions.filters.http.golang.v3.Config
  so_id: %s
  plugin_name: xx
  merge_policy: MERGE_VIRTUALHOST_ROUTER_FILTER
  plugin_config:
    "@type": type.googleapis.com/udpa.type.v1.TypedStruct
    type_url: typexx
    value:
        key: value
        int: 10
)EOF";

    auto yaml_string = absl::StrFormat(yaml_fmt, so_id);
    initializeFilter(yaml_string);
  }

  void initializeWithYaml(const std::string& filter_config, const std::string& route_config) {
    config_helper_.prependFilter(filter_config);

    createClusters();
    config_helper_.addConfigModifier(
        [route_config](
            envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager&
                hcm) { TestUtility::loadFromYaml(route_config, *hcm.mutable_route_config()); });
    initialize();
  }

  void createClusters() {
    config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
      auto* golang_cluster = bootstrap.mutable_static_resources()->add_clusters();
      golang_cluster->MergeFrom(bootstrap.static_resources().clusters()[0]);
      golang_cluster->set_name("golang_cluster");

      auto* alt_cluster = bootstrap.mutable_static_resources()->add_clusters();
      alt_cluster->MergeFrom(bootstrap.static_resources().clusters()[0]);
      alt_cluster->set_name("alt_cluster");

      auto* xds_cluster = bootstrap.mutable_static_resources()->add_clusters();
      xds_cluster->MergeFrom(bootstrap.static_resources().clusters()[0]);
      xds_cluster->set_name("xds_cluster");
      ConfigHelper::setHttp2(*xds_cluster);
    });
  }

  std::string genSoPath(std::string name) {
    // TODO: should work without the "_go_extension" suffix
    return TestEnvironment::substitute(
        "{{ test_rundir }}_go_extension/test/http/golang/test_data/" + name + "/filter.so");
  }

  void addDso(const std::string& so_id) {
    auto yaml_fmt = R"EOF(
name: envoy.bootstrap.dso
typed_config:
  "@type": type.googleapis.com/envoy.extensions.dso.v3.dso
  so_id: %s
  so_path: "%s"
)EOF";
    auto path = genSoPath(so_id);
    auto config = absl::StrFormat(yaml_fmt, so_id, path);

    config_helper_.addBootstrapExtension(config);
  }

  void initializeWithRds(const std::string& filter_config, const std::string& route_config_name,
                         const std::string& initial_route_config) {
    config_helper_.prependFilter(filter_config);

    // Create static clusters.
    createClusters();

    // Set RDS config source.
    config_helper_.addConfigModifier(
        [route_config_name](
            envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager&
                hcm) {
          hcm.mutable_rds()->set_route_config_name(route_config_name);
          hcm.mutable_rds()->mutable_config_source()->set_resource_api_version(
              envoy::config::core::v3::ApiVersion::V3);
          envoy::config::core::v3::ApiConfigSource* rds_api_config_source =
              hcm.mutable_rds()->mutable_config_source()->mutable_api_config_source();
          rds_api_config_source->set_api_type(envoy::config::core::v3::ApiConfigSource::GRPC);
          rds_api_config_source->set_transport_api_version(envoy::config::core::v3::V3);
          envoy::config::core::v3::GrpcService* grpc_service =
              rds_api_config_source->add_grpc_services();
          grpc_service->mutable_envoy_grpc()->set_cluster_name("xds_cluster");
        });

    on_server_init_function_ = [&]() {
      AssertionResult result =
          fake_upstreams_[3]->waitForHttpConnection(*dispatcher_, xds_connection_);
      RELEASE_ASSERT(result, result.message());
      result = xds_connection_->waitForNewStream(*dispatcher_, xds_stream_);
      RELEASE_ASSERT(result, result.message());
      xds_stream_->startGrpcStream();

      EXPECT_TRUE(compareSotwDiscoveryRequest(Config::TypeUrl::get().RouteConfiguration, "",
                                              {route_config_name}, true));
      sendSotwDiscoveryResponse<envoy::config::route::v3::RouteConfiguration>(
          Config::TypeUrl::get().RouteConfiguration,
          {TestUtility::parseYaml<envoy::config::route::v3::RouteConfiguration>(
              initial_route_config)},
          "1");
    };
    initialize();
    registerTestServerPorts({"http"});
  }

  void expectResponseBodyRewrite(const std::string& code, bool empty_body, bool enable_wrap_body) {
    initializeFilter(code);
    codec_client_ = makeHttpConnection(makeClientConnection(lookupPort("http")));
    Http::TestRequestHeaderMapImpl request_headers{{":method", "POST"},
                                                   {":path", "/test"},
                                                   {":scheme", "http"},
                                                   {":authority", "host"},
                                                   {"x-forwarded-for", "10.0.0.1"}};

    auto encoder_decoder = codec_client_->startRequest(request_headers);
    Http::StreamEncoder& encoder = encoder_decoder.first;
    auto response = std::move(encoder_decoder.second);
    Buffer::OwnedImpl request_data("done");
    encoder.encodeData(request_data, true);

    waitForNextUpstreamRequest();

    Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}, {"foo", "bar"}};

    if (empty_body) {
      upstream_request_->encodeHeaders(response_headers, true);
    } else {
      upstream_request_->encodeHeaders(response_headers, false);
      Buffer::OwnedImpl response_data1("good");
      upstream_request_->encodeData(response_data1, false);
      Buffer::OwnedImpl response_data2("bye");
      upstream_request_->encodeData(response_data2, true);
    }

    ASSERT_TRUE(response->waitForEndStream());

    if (enable_wrap_body) {
      EXPECT_EQ("2", response->headers()
                         .get(Http::LowerCaseString("content-length"))[0]
                         ->value()
                         .getStringView());
      EXPECT_EQ("ok", response->body());
    } else {
      EXPECT_EQ("", response->body());
    }

    cleanup();
  }

  void testRewriteResponse(const std::string& code) {
    expectResponseBodyRewrite(code, false, true);
  }

  void testRewriteResponseWithoutUpstreamBody(const std::string& code, bool enable_wrap_body) {
    expectResponseBodyRewrite(code, true, enable_wrap_body);
  }

  void testBasic(std::string path) {
    initializeSimpleFilter(BASIC);

    codec_client_ = makeHttpConnection(makeClientConnection(lookupPort("http")));
    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "POST"},        {":path", path},
        {":scheme", "http"},        {":authority", "host"},
        {"x-test-header-0", "foo"}, {"x-test-header-1", "bar"}};

    auto encoder_decoder = codec_client_->startRequest(request_headers);
    Http::StreamEncoder& encoder = encoder_decoder.first;
    auto response = std::move(encoder_decoder.second);
    Buffer::OwnedImpl request_data1("hello");
    encoder.encodeData(request_data1, false);
    Buffer::OwnedImpl request_data2("world");
    encoder.encodeData(request_data2, false);
    Buffer::OwnedImpl request_data3("");
    encoder.encodeData(request_data3, true);

    waitForNextUpstreamRequest();
    // original header: x-test-header-0
    EXPECT_EQ("foo", upstream_request_->headers()
                         .get(Http::LowerCaseString("x-test-header-0"))[0]
                         ->value()
                         .getStringView());

    // check header value which set in golang: test-x-set-header-0
    EXPECT_EQ("foo", upstream_request_->headers()
                         .get(Http::LowerCaseString("test-x-set-header-0"))[0]
                         ->value()
                         .getStringView());

    // check header exists which removed in golang side: x-test-header-1
    EXPECT_EQ(true,
              upstream_request_->headers().get(Http::LowerCaseString("x-test-header-1")).empty());

    // upper("helloworld")
    EXPECT_EQ("HELLOWORLD", upstream_request_->body().toString());

    Http::TestResponseHeaderMapImpl response_headers{
        {":status", "200"}, {"x-test-header-0", "foo"}, {"x-test-header-1", "bar"}};
    upstream_request_->encodeHeaders(response_headers, false);
    Buffer::OwnedImpl response_data1("good");
    upstream_request_->encodeData(response_data1, false);
    Buffer::OwnedImpl response_data2("bye");
    upstream_request_->encodeData(response_data2, true);

    ASSERT_TRUE(response->waitForEndStream());

    // original resp header: x-test-header-0
    EXPECT_EQ("foo", response->headers()
                         .get(Http::LowerCaseString("x-test-header-0"))[0]
                         ->value()
                         .getStringView());

    // check resp header value which set in golang: test-x-set-header-0
    EXPECT_EQ("foo", response->headers()
                         .get(Http::LowerCaseString("test-x-set-header-0"))[0]
                         ->value()
                         .getStringView());

    // check resp header exists which removed in golang side: x-test-header-1
    EXPECT_EQ(true, response->headers().get(Http::LowerCaseString("x-test-header-1")).empty());

    // length("helloworld") = 10
    EXPECT_EQ("10", response->headers()
                        .get(Http::LowerCaseString("test-req-body-length"))[0]
                        ->value()
                        .getStringView());

    // verify path
    EXPECT_EQ(
        path,
        response->headers().get(Http::LowerCaseString("test-path"))[0]->value().getStringView());

    // upper("goodbye")
    EXPECT_EQ("GOODBYE", response->body());

    cleanup();
  }

  void cleanup() {
    codec_client_->close();
    if (fake_golang_connection_ != nullptr) {
      AssertionResult result = fake_golang_connection_->close();
      RELEASE_ASSERT(result, result.message());
      result = fake_golang_connection_->waitForDisconnect();
      RELEASE_ASSERT(result, result.message());
    }
    if (fake_upstream_connection_ != nullptr) {
      AssertionResult result = fake_upstream_connection_->close();
      RELEASE_ASSERT(result, result.message());
      result = fake_upstream_connection_->waitForDisconnect();
      RELEASE_ASSERT(result, result.message());
    }
    if (xds_connection_ != nullptr) {
      AssertionResult result = xds_connection_->close();
      RELEASE_ASSERT(result, result.message());
      result = xds_connection_->waitForDisconnect();
      RELEASE_ASSERT(result, result.message());
      xds_connection_ = nullptr;
    }
  }

  FakeHttpConnectionPtr fake_golang_connection_;
  FakeStreamPtr golang_request_;

  const std::string BASIC{"basic"};
  const std::string ASYNC{"async"};
};

INSTANTIATE_TEST_SUITE_P(IpVersions, GolangIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

TEST_P(GolangIntegrationTest, BASIC) { testBasic("/test"); }

TEST_P(GolangIntegrationTest, ASYNC) { testBasic("/test?async=1"); }

TEST_P(GolangIntegrationTest, SLEEP) { testBasic("/test?sleep=1"); }

TEST_P(GolangIntegrationTest, ASYNC_SLEEP) { testBasic("/test?async=1&sleep=1"); }

TEST_P(GolangIntegrationTest, DATASLEEP) { testBasic("/test?data_sleep=1"); }

TEST_P(GolangIntegrationTest, ASYNC_DATASLEEP) { testBasic("/test?async=1&data_sleep=1"); }

} // namespace
} // namespace Envoy
