#include "fastmcpp/util/ui.hpp"

namespace fastmcpp::util::ui
{

const char* const FASTMCP_LOGO_URL = "https://gofastmcp.com/assets/brand/blue-logo.png";

// clang-format off
const char* const BASE_STYLES = R"CSS(
    * {
        margin: 0;
        padding: 0;
        box-sizing: border-box;
    }

    body {
        font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, sans-serif;
        margin: 0;
        padding: 0;
        min-height: 100vh;
        display: flex;
        align-items: center;
        justify-content: center;
        background: #f9fafb;
        color: #0a0a0a;
    }

    .container {
        background: #ffffff;
        border: 1px solid #e5e7eb;
        padding: 3rem 2.5rem;
        border-radius: 1rem;
        box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1), 0 2px 4px -1px rgba(0, 0, 0, 0.06);
        text-align: center;
        max-width: 36rem;
        margin: 1rem;
        width: 100%;
    }

    @media (max-width: 640px) {
        .container {
            padding: 2rem 1.5rem;
            margin: 0.5rem;
        }
    }

    .logo {
        width: 64px;
        height: auto;
        margin-bottom: 1.5rem;
        display: block;
        margin-left: auto;
        margin-right: auto;
    }

    h1 {
        font-size: 1.5rem;
        font-weight: 600;
        margin-bottom: 1.5rem;
        color: #111827;
    }
)CSS";

const char* const BUTTON_STYLES = R"CSS(
    .button-group {
        display: flex;
        gap: 0.75rem;
        margin-top: 1.5rem;
        justify-content: center;
    }

    button {
        padding: 0.75rem 2rem;
        font-size: 0.9375rem;
        font-weight: 500;
        border-radius: 0.5rem;
        border: none;
        cursor: pointer;
        transition: all 0.15s;
        font-family: inherit;
    }

    button:hover {
        transform: translateY(-1px);
        box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1);
    }

    .btn-approve, .btn-primary {
        background: #10b981;
        color: #ffffff;
        min-width: 120px;
    }

    .btn-deny, .btn-secondary {
        background: #6b7280;
        color: #ffffff;
        min-width: 120px;
    }
)CSS";

const char* const DETAILS_STYLES = R"CSS(
    details {
        margin-bottom: 1.5rem;
        text-align: left;
    }

    summary {
        cursor: pointer;
        font-size: 0.875rem;
        color: #6b7280;
        font-weight: 600;
        list-style: none;
        padding: 0.5rem;
        border-radius: 0.25rem;
    }

    summary:hover {
        background: #f9fafb;
    }

    summary::marker {
        display: none;
    }

    summary::before {
        content: "▶";
        display: inline-block;
        margin-right: 0.5rem;
        transition: transform 0.2s;
        font-size: 0.75rem;
    }

    details[open] summary::before {
        transform: rotate(90deg);
    }
)CSS";

const char* const DETAIL_BOX_STYLES = R"CSS(
    .detail-box {
        background: #f9fafb;
        border: 1px solid #e5e7eb;
        border-radius: 0.5rem;
        padding: 1rem;
        margin-bottom: 1.5rem;
        text-align: left;
    }

    .detail-row {
        display: flex;
        padding: 0.5rem 0;
        border-bottom: 1px solid #e5e7eb;
    }

    .detail-row:last-child {
        border-bottom: none;
    }

    .detail-label {
        font-weight: 600;
        min-width: 160px;
        color: #6b7280;
        font-size: 0.875rem;
        flex-shrink: 0;
        padding-right: 1rem;
    }

    .detail-value {
        flex: 1;
        font-family: 'SF Mono', 'Monaco', 'Consolas', 'Courier New', monospace;
        font-size: 0.75rem;
        color: #111827;
        word-break: break-all;
        overflow-wrap: break-word;
    }
)CSS";


const char* const HELPER_TEXT_STYLES = R"CSS(
    .close-instruction, .help-text {
        font-size: 0.875rem;
        color: #6b7280;
        margin-top: 1.5rem;
    }
)CSS";

const char* const INFO_BOX_STYLES = R"CSS(
    .info-box {
        background: #f0f9ff;
        border: 1px solid #bae6fd;
        border-radius: 0.5rem;
        padding: 1rem;
        margin-bottom: 1.5rem;
        text-align: left;
        font-size: 0.9375rem;
        line-height: 1.5;
        color: #374151;
    }

    .info-box p {
        margin-bottom: 0.5rem;
    }

    .info-box p:last-child {
        margin-bottom: 0;
    }

    .info-box.centered {
        text-align: center;
    }

    .info-box.error {
        background: #fef2f2;
        border-color: #fecaca;
        color: #991b1b;
    }

    .info-box strong {
        color: #0ea5e9;
        font-weight: 600;
    }

    .info-box .server-name-link {
        color: #0ea5e9;
        text-decoration: underline;
        font-weight: 600;
        cursor: pointer;
        transition: opacity 0.15s;
    }

    .info-box .server-name-link:hover {
        opacity: 0.8;
    }

    /* Monospace info box - gray styling with code font */
    .info-box-mono {
        background: #f9fafb;
        border: 1px solid #e5e7eb;
        border-radius: 0.5rem;
        padding: 0.875rem;
        margin: 1.25rem 0;
        font-size: 0.875rem;
        color: #6b7280;
        font-family: 'SF Mono', 'Monaco', 'Consolas', 'Courier New', monospace;
        text-align: left;
    }

    .info-box-mono.centered {
        text-align: center;
    }

    .info-box-mono.error {
        background: #fef2f2;
        border-color: #fecaca;
        color: #991b1b;
    }

    .info-box-mono strong {
        color: #111827;
        font-weight: 600;
    }

    .warning-box {
        background: #f0f9ff;
        border: 1px solid #bae6fd;
        border-radius: 0.5rem;
        padding: 1rem;
        margin-bottom: 1.5rem;
        text-align: center;
    }

    .warning-box p {
        margin-bottom: 0.5rem;
        line-height: 1.5;
        color: #6b7280;
        font-size: 0.9375rem;
    }

    .warning-box p:last-child {
        margin-bottom: 0;
    }

    .warning-box strong {
        color: #0ea5e9;
        font-weight: 600;
    }

    .warning-box a {
        color: #0ea5e9;
        text-decoration: underline;
        font-weight: 600;
    }

    .warning-box a:hover {
        color: #0284c7;
        text-decoration: underline;
    }
)CSS";

const char* const REDIRECT_SECTION_STYLES = R"CSS(
    .redirect-section {
        background: #fffbeb;
        border: 1px solid #fcd34d;
        border-radius: 0.5rem;
        padding: 1rem;
        margin-bottom: 1.5rem;
        text-align: left;
    }

    .redirect-section .label {
        font-size: 0.875rem;
        color: #6b7280;
        font-weight: 600;
        margin-bottom: 0.5rem;
        display: block;
    }

    .redirect-section .value {
        font-family: 'SF Mono', 'Monaco', 'Consolas', 'Courier New', monospace;
        font-size: 0.875rem;
        color: #111827;
        word-break: break-all;
        margin-top: 0.25rem;
    }
)CSS";

const char* const STATUS_MESSAGE_STYLES = R"CSS(
    .status-message {
        display: flex;
        align-items: center;
        justify-content: center;
        gap: 0.75rem;
        margin-bottom: 1.5rem;
    }

    .status-icon {
        font-size: 1.5rem;
        line-height: 1;
        display: inline-flex;
        align-items: center;
        justify-content: center;
        width: 2rem;
        height: 2rem;
        border-radius: 0.5rem;
        flex-shrink: 0;
    }

    .status-icon.success {
        background: #10b98120;
    }

    .status-icon.error {
        background: #ef444420;
    }

    .message {
        font-size: 1.125rem;
        line-height: 1.75;
        color: #111827;
        font-weight: 600;
        text-align: left;
    }
)CSS";

const char* const TOOLTIP_STYLES = R"CSS(
    .help-link-container {
        position: fixed;
        bottom: 1.5rem;
        right: 1.5rem;
        font-size: 0.875rem;
    }

    .help-link {
        color: #6b7280;
        text-decoration: none;
        cursor: help;
        position: relative;
        display: inline-block;
        border-bottom: 1px dotted #9ca3af;
    }

    @media (max-width: 640px) {
        .help-link {
            background: #ffffff;
            padding: 0.25rem 0.5rem;
            border-radius: 0.25rem;
            box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1);
        }
    }

    .help-link:hover {
        color: #111827;
        border-bottom-color: #111827;
    }

    .help-link:hover .tooltip {
        opacity: 1;
        visibility: visible;
    }

    .tooltip {
        position: absolute;
        bottom: 100%;
        right: 0;
        left: auto;
        margin-bottom: 0.5rem;
        background: #1f2937;
        color: #ffffff;
        padding: 0.75rem 1rem;
        border-radius: 0.5rem;
        font-size: 0.8125rem;
        line-height: 1.5;
        width: 280px;
        max-width: calc(100vw - 3rem);
        opacity: 0;
        visibility: hidden;
        transition: opacity 0.2s, visibility 0.2s;
        box-shadow: 0 10px 15px -3px rgba(0, 0, 0, 0.1);
        text-align: left;
    }

    .tooltip::after {
        content: '';
        position: absolute;
        top: 100%;
        right: 1rem;
        border: 6px solid transparent;
        border-top-color: #1f2937;
    }

    .tooltip-link {
        color: #60a5fa;
        text-decoration: underline;
    }
)CSS";

// clang-format on

const char* const DEFAULT_CSP_POLICY =
    "default-src 'none'; style-src 'unsafe-inline'; img-src https: data:; base-uri 'none'";

std::string html_escape(const std::string& text, bool quote)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += quote ? "&quot;" : "\"";
            break;
        case '\'':
            out += quote ? "&#x27;" : "'";
            break;
        default:
            out.push_back(c);
        }
    return out;
}

std::string create_page(const std::string& content, const std::string& title,
                        const std::string& additional_styles, const std::string& csp_policy)
{
    const std::string csp_meta = csp_policy.empty()
                                     ? std::string()
                                     : "<meta http-equiv=\"Content-Security-Policy\" content=\"" +
                                           html_escape(csp_policy, true) + "\" />";
    return "\n    <!DOCTYPE html>\n    <html lang=\"en\">\n    <head>\n        <meta "
           "charset=\"UTF-8\">\n        <meta name=\"viewport\" content=\"width=device-width, "
           "initial-scale=1.0\">\n        <title>" +
           html_escape(title) + "</title>\n        <style>\n            " + BASE_STYLES +
           "\n            " + additional_styles + "\n        </style>\n        " + csp_meta +
           "\n    </head>\n    <body>\n        " + content + "\n    </body>\n    </html>\n    ";
}

std::string create_logo(const std::optional<std::string>& icon_url, const std::string& alt_text)
{
    const std::string url = icon_url && !icon_url->empty() ? *icon_url : FASTMCP_LOGO_URL;
    return "<img src=\"" + html_escape(url) + "\" alt=\"" + html_escape(alt_text) +
           "\" class=\"logo\" />";
}

std::string create_status_message(const std::string& message, bool is_success)
{
    return "\n        <div class=\"status-message\">\n            <span class=\"status-icon " +
           std::string(is_success ? "success" : "error") + "\">" +
           (is_success ? "\xE2\x9C\x93" : "\xE2\x9C\x95") +
           "</span>\n            <div class=\"message\">" + html_escape(message) +
           "</div>\n        </div>\n    ";
}

std::string create_info_box(const std::string& content, bool is_error, bool centered,
                            bool monospace)
{
    std::string classes = monospace ? "info-box-mono" : "info-box";
    if (is_error)
        classes += " error";
    if (centered)
        classes += " centered";
    return "<div class=\"" + classes + "\">" + html_escape(content) + "</div>";
}

std::string create_detail_box(const std::vector<std::pair<std::string, std::string>>& rows)
{
    std::string rows_html;
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        if (i)
            rows_html += "\n";
        rows_html += "\n        <div class=\"detail-row\">\n            <div "
                     "class=\"detail-label\">" +
                     html_escape(rows[i].first) +
                     ":</div>\n            <div class=\"detail-value\">" +
                     html_escape(rows[i].second) + "</div>\n        </div>\n        ";
    }
    return "<div class=\"detail-box\">" + rows_html + "</div>";
}

std::string
create_button_group(const std::vector<std::tuple<std::string, std::string, std::string>>& buttons)
{
    std::string html;
    for (std::size_t i = 0; i < buttons.size(); ++i)
    {
        const auto& [text, value, css_class] = buttons[i];
        if (i)
            html += "\n";
        html += "<button type=\"submit\" name=\"action\" value=\"" + value + "\" class=\"" +
                css_class + "\">" + text + "</button>";
    }
    return "<div class=\"button-group\">" + html + "</div>";
}

std::vector<std::pair<std::string, std::string>> secure_html_headers()
{
    return {{"X-Frame-Options", "DENY"}};
}

} // namespace fastmcpp::util::ui