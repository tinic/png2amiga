#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace png2amiga {

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

enum class ErrorCode : std::uint8_t {
    file_not_found,
    invalid_png,
    invalid_dimensions,
    write_failed,
    unsupported_mode,
    invalid_depth,
};

struct Error {
    ErrorCode code;
    std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

// ---------------------------------------------------------------------------
// Color3f -- linear RGB color
// ---------------------------------------------------------------------------

struct Color3f {
    float r{};
    float g{};
    float b{};

    constexpr auto operator<=>(const Color3f&) const = default;

    constexpr Color3f operator+(Color3f rhs) const noexcept {
        return {r + rhs.r, g + rhs.g, b + rhs.b};
    }

    constexpr Color3f operator-(Color3f rhs) const noexcept {
        return {r - rhs.r, g - rhs.g, b - rhs.b};
    }

    constexpr Color3f operator*(float s) const noexcept {
        return {r * s, g * s, b * s};
    }

    constexpr Color3f& operator+=(Color3f rhs) noexcept {
        r += rhs.r;
        g += rhs.g;
        b += rhs.b;
        return *this;
    }

    constexpr Color3f& operator-=(Color3f rhs) noexcept {
        r -= rhs.r;
        g -= rhs.g;
        b -= rhs.b;
        return *this;
    }

    constexpr Color3f& operator*=(float s) noexcept {
        r *= s;
        g *= s;
        b *= s;
        return *this;
    }

    friend constexpr Color3f operator*(float s, Color3f c) noexcept {
        return c * s;
    }

    constexpr Color3f clamped() const noexcept {
        return {
            std::clamp(r, 0.0f, 1.0f),
            std::clamp(g, 0.0f, 1.0f),
            std::clamp(b, 0.0f, 1.0f),
        };
    }
};

// ---------------------------------------------------------------------------
// Concepts
// ---------------------------------------------------------------------------

template <typename T>
concept ColorType = requires(T c) {
    { c.r } -> std::convertible_to<float>;
    { c.g } -> std::convertible_to<float>;
    { c.b } -> std::convertible_to<float>;
};

// ---------------------------------------------------------------------------
// Image -- owning 2D pixel buffer
// ---------------------------------------------------------------------------

class Image {
    std::vector<Color3f> pixels_;
    std::vector<float> alpha_;      // empty if fully opaque
    std::size_t width_{};
    std::size_t height_{};

public:
    Image() = default;

    Image(std::size_t w, std::size_t h)
        : pixels_(w * h), width_(w), height_(h) {}

    Image(std::size_t w, std::size_t h, std::vector<Color3f> data)
        : pixels_(std::move(data)), width_(w), height_(h) {}

    Image(std::size_t w, std::size_t h, std::vector<Color3f> data,
          std::vector<float> a)
        : pixels_(std::move(data)), alpha_(std::move(a)),
          width_(w), height_(h) {}

    [[nodiscard]] std::size_t width() const noexcept { return width_; }
    [[nodiscard]] std::size_t height() const noexcept { return height_; }
    [[nodiscard]] bool has_alpha() const noexcept { return !alpha_.empty(); }

    [[nodiscard]] float alpha_at(std::size_t x, std::size_t y) const noexcept {
        if (alpha_.empty()) return 1.0f;
        return alpha_[y * width_ + x];
    }

    [[nodiscard]] std::span<const float> alpha() const noexcept {
        return alpha_;
    }

    void set_alpha(std::vector<float> a) { alpha_ = std::move(a); }

    Color3f& operator[](std::size_t x, std::size_t y) noexcept {
        return pixels_[y * width_ + x];
    }

    const Color3f& operator[](std::size_t x, std::size_t y) const noexcept {
        return pixels_[y * width_ + x];
    }

    [[nodiscard]] std::span<Color3f> row(std::size_t y) noexcept {
        return {pixels_.data() + y * width_, width_};
    }

    [[nodiscard]] std::span<const Color3f> row(std::size_t y) const noexcept {
        return {pixels_.data() + y * width_, width_};
    }

    [[nodiscard]] std::span<Color3f> pixels() noexcept {
        return pixels_;
    }

    [[nodiscard]] std::span<const Color3f> pixels() const noexcept {
        return pixels_;
    }
};

// ---------------------------------------------------------------------------
// Palette -- named collection of colors
// ---------------------------------------------------------------------------

struct Palette {
    std::string name;
    std::vector<Color3f> colors;

    [[nodiscard]] std::span<const Color3f> as_span() const noexcept {
        return colors;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return colors.size();
    }
};

} // namespace png2amiga
