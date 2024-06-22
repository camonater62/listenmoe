#include <iostream>
#include <thread>
#include <chrono>
#include <iomanip>
#include <string>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <cstdint>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "stb_vorbis.c"
#include <functional>
#include <mutex>

using namespace std;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

mutex rawdata_mutex;

void data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount)
{
    lock_guard guard(rawdata_mutex);
    // cout << "Callback called, reading " << frameCount << " frames" << endl;
    vector<vector<float>> &rawdata = *(vector<vector<float>> *)pDevice->pUserData;
    if (rawdata.size() == 0 || rawdata[0].size() < frameCount) {
        return;
    }
    int index = 0;
    for (int i = 0; i < frameCount; i++)
    {
        for (const auto &channel : rawdata)
        {
            if (i >= channel.size())
            {
                return;
            }
            else
            {
                ((float *)(pOutput))[index] = channel[i];
            }
            index++;
        }
    }
    for (auto &channel : rawdata)
    {
        int toRemove = min(channel.size(), (size_t)frameCount);
        channel.erase(channel.begin(), channel.begin() + toRemove);
    }
};

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

        http::parser<false, http::buffer_body> p;
        http::read_header(stream, buffer, p);

        beast::error_code ec{};

        vector<uint8_t> vorbisdata;
        stb_vorbis *vorbis = nullptr;

        vector<vector<float>> rawdata;

        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 2;
        config.sampleRate = 44100;
        config.dataCallback = data_callback;
        config.pUserData = (void *)&rawdata;

        ma_device device;
        if (ma_device_init(NULL, &config, &device) != MA_SUCCESS)
        {
            cerr << "Failed to initialize miniaudio device :(" << endl;
            return -1;
        }

        bool ma_device_started = false;
        constexpr size_t BUFFER_SIZE = 10 * 1024 * 1024;
        uint8_t *buf = new uint8_t[BUFFER_SIZE];

        while (!p.is_done())
        {
            p.get().body().data = buf;
            p.get().body().size = BUFFER_SIZE;
            size_t read = http::read_some(stream, buffer, p, ec);
            if (ec && ec != http::error::need_buffer)
            {
                break;
            }
            for (int i = 0; i < read; i++)
            {
                vorbisdata.push_back(buf[i]);
            }
            if (vorbis == nullptr)
            {
                const uint8_t *datablock = vorbisdata.data();
                int datablock_length = vorbisdata.size();
                int datablock_memory_consumed = 0;        // Filled by function
                int error = 0;                            // Filled by function
                stb_vorbis_alloc *alloc_buffer = nullptr; // Use malloc
                vorbis = stb_vorbis_open_pushdata(datablock,
                                                  datablock_length,
                                                  &datablock_memory_consumed,
                                                  &error,
                                                  alloc_buffer);
                if (vorbis == nullptr && error != STBVorbisError::VORBIS_need_more_data)
                {
                    cerr << "Got error during decode: " << error << endl;
                }
                else
                {
                    vorbisdata.erase(vorbisdata.begin(),
                                     vorbisdata.begin() + datablock_memory_consumed);
                }
            }
            else
            {
                // TODO: Handle comments
                lock_guard guard(rawdata_mutex);
                bool has_more_to_read = true;
                while (has_more_to_read)
                {
                    const uint8_t *datablock = vorbisdata.data();
                    int datablock_length = vorbisdata.size();
                    int channels = 0;         // To be filled by function
                    float **output = nullptr; // To be filled by function
                    int samples = 0;          // To be filled by function
                    int bytes_read = stb_vorbis_decode_frame_pushdata(vorbis,
                                                                      datablock,
                                                                      datablock_length,
                                                                      &channels,
                                                                      &output,
                                                                      &samples);
                    if (bytes_read != 0 && channels != 0)
                    {
                        // cout << "Buffering up " << samples << " samples" << endl;
                        while (channels > rawdata.size())
                        {
                            rawdata.push_back(vector<float>());
                        }
                        for (int j = 0; j < channels; j++)
                        {
                            for (int i = 0; i < samples; i++)
                            {
                                rawdata[j].push_back(output[j][i]);
                            }
                        }
                        vorbisdata.erase(vorbisdata.begin(),
                                         vorbisdata.begin() + bytes_read);
                        // cout << "Rawdata size is " << rawdata[0].size() << endl;
                    }
                    else
                    {
                        has_more_to_read = false;
                    }
                }
            }
            if (!ma_device_started && rawdata.size() > 0 && rawdata[0].size() > 44100)
            {
                // cout << "Starting up ma device" << endl;
                ma_device_start(&device);
            }
        }
        ma_device_uninit(&device);
        delete[] buf;
    }
    catch (std::exception const &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}