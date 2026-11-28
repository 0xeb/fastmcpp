#pragma once
/// @file util/ui.hpp
/// @brief HTML building blocks for the pages fastmcpp serves to browsers:
///        OAuth callback, consent and error pages.

#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace fastmcpp::util::ui
{

extern const char* const FASTMCP_LOGO_URL;
extern const char* const BASE_STYLES;
extern const char* const BUTTON_STYLES;
extern const char* const INFO_BOX_STYLES;
extern const char* const STATUS_MESSAGE_STYLES;
extern const char* const DETAIL_BOX_STYLES;
extern const char* const REDIRECT_SECTION_STYLES;
extern const char* const DETAILS_STYLES;
extern const char* const HELPER_TEXT_STYLES;
extern const char* const TOOLTIP_STYLES;

/// The Content-Security-Policy create_page() applies by default.
extern const char* const DEFAULT_CSP_POLICY;

/// HTML-escape &, <, > and, with @p quote, " and '.
std::string html_escape(const std::string& text, bool quote = true);

/// A complete page with the base styles plus @p additional_styles. An empty
/// @p csp_policy omits the Content-Security-Policy meta tag.
std::string create_page(const std::string& content, const std::string& title = "FastMCP",
                        const std::string& additional_styles = "",
                        const std::string& csp_policy = DEFAULT_CSP_POLICY);

/// A logo image; the FastMCP logo when @p icon_url is empty.
std::string create_logo(const std::optional<std::string>& icon_url = std::nullopt,
                        const std::string& alt_text = "FastMCP");

/// A status line with a check mark (success) or a cross.
std::string create_status_message(const std::string& message, bool is_success = true);

/// An info box with escaped @p content.
std::string create_info_box(const std::string& content, bool is_error = false,
                            bool centered = false, bool monospace = false);

/// A box of escaped "label: value" rows.
std::string create_detail_box(const std::vector<std::pair<std::string, std::string>>& rows);

/// Submit buttons, each (text, value, css class).
std::string
create_button_group(const std::vector<std::tuple<std::string, std::string, std::string>>& buttons);

/// Headers to send with an HTML page: X-Frame-Options DENY against clickjacking.
std::vector<std::pair<std::string, std::string>> secure_html_headers();

} // namespace fastmcpp::util::ui
