#include <iostream>
#include <thread>
#include <chrono>
#include <iomanip>
#include <string>
#include <cstring>
#include <cstdlib>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>

using namespace std;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

const char *STREAM_URL = "https://listen.moe/kpop/stream";
// const char* STREAM_URL = "https://example.com/";

int main()
{
    try
    {
        auto const host = "listen.moe";
        auto const port = "443";
        auto const target = "/kpop/stream";
        int version = 10;

        net::io_context ioc;

        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.set_verify_mode(ssl::verify_none);

        tcp::resolver resolver(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

        if (!SSL_set_tlsext_host_name(stream.native_handle(), host))
        {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
            throw beast::system_error{ec};
        }

        auto const results = resolver.resolve(host, port);

        beast::get_lowest_layer(stream).connect(results);

        stream.handshake(ssl::stream_base::client);

        http::request<http::string_body> req{http::verb::get, target, version};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

        http::write(stream, req);

        beast::flat_buffer buffer;
        

        // http::response<http::dynamic_body> res;
        // http::read(stream, buffer, res);
        http::parser<false, http::buffer_body> p;
        http::read_header(stream, buffer, p);
        cerr << "Read " << buffer.size() << " bytes :)" << endl;
        
        beast::error_code ec{};
        while (!p.is_done())
        {
            char buf[1024] = {0};
            p.get().body().data = buf;
            p.get().body().size = sizeof(buf);
            size_t read = http::read_some(stream, buffer, p, ec);
            cerr << "Read " << read << " bytes :) " << endl;
            if (ec && ec != http::error::need_buffer)
                break;
            for (int i = 0; i < read; i++)
                cout << buf[i];
            cout.flush();
        }
    }
    catch (std::exception const &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}