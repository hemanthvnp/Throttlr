/**
 * @file compression.cpp
 * @brief Response compression middleware
 */

#include "gateway/middleware/compression.hpp"
#include <zlib.h>

namespace gateway::middleware {

CompressionMiddleware::CompressionMiddleware(Config config)
    : config_(std::move(config)) {}

MiddlewareResult CompressionMiddleware::on_response(Request& request, Response& response) {
    // Check if compression is appropriate
    if (response.body().size() < config_.min_size) {
        return {MiddlewareAction::Continue, nullptr};
    }

    // Check content type
    auto content_type = response.header("Content-Type");
    if (content_type && !should_compress(*content_type)) {
        return {MiddlewareAction::Continue, nullptr};
    }

    // Check Accept-Encoding
    auto accept = request.header("Accept-Encoding");
    if (!accept) {
        return {MiddlewareAction::Continue, nullptr};
    }

    std::string encoding;
    if (accept->find("br") != std::string::npos &&
        std::find(config_.algorithms.begin(), config_.algorithms.end(), "br")
            != config_.algorithms.end()) {
        encoding = "br";
    } else if (accept->find("gzip") != std::string::npos &&
               std::find(config_.algorithms.begin(), config_.algorithms.end(), "gzip")
                   != config_.algorithms.end()) {
        encoding = "gzip";
    }

    if (encoding.empty()) {
        return {MiddlewareAction::Continue, nullptr};
    }

    // Compress
    std::string compressed;
    if (encoding == "gzip") {
        compressed = gzip_compress(response.body());
    } else if (encoding == "br") {
        compressed = brotli_compress(response.body());
    }

    if (!compressed.empty() && compressed.size() < response.body().size()) {
        response.set_body(std::move(compressed));
        response.set_header("Content-Encoding", encoding);
        response.set_header("Vary", "Accept-Encoding");
    }

    return {MiddlewareAction::Continue, nullptr};
}

bool CompressionMiddleware::should_compress(std::string_view content_type) const {
    static const std::vector<std::string> compressible = {
        "text/", "application/json", "application/xml",
        "application/javascript", "application/x-javascript",
        "image/svg+xml"
    };

    for (const auto& prefix : compressible) {
        if (content_type.find(prefix) != std::string::npos) {
            return true;
        }
    }

    return false;
}

std::string CompressionMiddleware::gzip_compress(const std::string& data) {
    z_stream zs{};
    if (deflateInit2(&zs, config_.level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return "";
    }

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());

    std::string output;
    output.resize(deflateBound(&zs, static_cast<uLong>(data.size())));

    zs.next_out = reinterpret_cast<Bytef*>(output.data());
    zs.avail_out = static_cast<uInt>(output.size());

    int ret = deflate(&zs, Z_FINISH);
    deflateEnd(&zs);

    if (ret != Z_STREAM_END) {
        return "";
    }

    output.resize(zs.total_out);
    return output;
}

std::string CompressionMiddleware::brotli_compress(const std::string& data) {
    // Brotli compression would be implemented here
    // Requires libbrotli
    return "";
}

} // namespace gateway::middleware
