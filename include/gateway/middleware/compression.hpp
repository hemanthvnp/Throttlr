#pragma once

/**
 * @file compression.hpp
 * @brief Response compression middleware (gzip, brotli)
 */

#include "gateway/middleware/middleware.hpp"
#include <zlib.h>

namespace gateway::middleware {

/**
 * @enum CompressionAlgorithm
 * @brief Supported compression algorithms
 */
enum class CompressionAlgorithm {
    None,
    Gzip,
    Deflate,
    Brotli
};

/**
 * @class CompressionMiddleware
 * @brief Response compression middleware
 *
 * Features:
 * - gzip compression
 * - Brotli compression
 * - Content-type based compression
 * - Minimum size threshold
 * - Accept-Encoding negotiation
 */
class CompressionMiddleware : public Middleware {
public:
    struct Config {
        std::vector<CompressionAlgorithm> algorithms{
            CompressionAlgorithm::Brotli,
            CompressionAlgorithm::Gzip
        };

        std::size_t min_size{1024};  // Minimum size to compress

        // Compression levels (algorithm-specific)
        int gzip_level{6};     // 1-9
        int brotli_level{4};   // 0-11

        // Content types to compress
        std::vector<std::string> compressible_types{
            "text/html",
            "text/css",
            "text/plain",
            "text/xml",
            "text/javascript",
            "application/json",
            "application/javascript",
            "application/xml",
            "application/xhtml+xml",
            "image/svg+xml"
        };

        // Don't compress if already has Content-Encoding
        bool skip_if_encoded{true};
    };

    explicit CompressionMiddleware(Config config = {});
    ~CompressionMiddleware() override;

    [[nodiscard]] std::string name() const override { return "compression"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::PreResponse; }
    [[nodiscard]] int priority() const override { return 90; }

    [[nodiscard]] MiddlewareResult on_response(Request& request, Response& response) override;

    // Compression utilities
    [[nodiscard]] static Result<ByteBuffer> compress_gzip(ByteSpan data, int level = 6);
    [[nodiscard]] static Result<ByteBuffer> decompress_gzip(ByteSpan data);
    [[nodiscard]] static Result<ByteBuffer> compress_brotli(ByteSpan data, int level = 4);
    [[nodiscard]] static Result<ByteBuffer> decompress_brotli(ByteSpan data);

private:
    [[nodiscard]] bool should_compress(const Request& request, const Response& response) const;
    [[nodiscard]] CompressionAlgorithm select_algorithm(const Request& request) const;
    [[nodiscard]] bool is_compressible_type(std::string_view content_type) const;
    [[nodiscard]] Result<void> compress_response(Response& response, CompressionAlgorithm algorithm);

    Config config_;
};

/**
 * @class DecompressionMiddleware
 * @brief Request body decompression middleware
 */
class DecompressionMiddleware : public Middleware {
public:
    explicit DecompressionMiddleware(std::size_t max_decompressed_size = 100 * 1024 * 1024);  // 100MB

    [[nodiscard]] std::string name() const override { return "decompression"; }
    [[nodiscard]] MiddlewarePhase phase() const override { return MiddlewarePhase::PreRoute; }
    [[nodiscard]] int priority() const override { return 15; }

    [[nodiscard]] MiddlewareResult on_request(Request& request) override;

private:
    std::size_t max_decompressed_size_;
};

} // namespace gateway::middleware
