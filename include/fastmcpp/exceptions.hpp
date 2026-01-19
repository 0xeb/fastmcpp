#pragma once
#include <stdexcept>
#include <string>

namespace fastmcpp
{

struct Error : public std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct NotFoundError : public Error
{
    using Error::Error;
};

struct ValidationError : public Error
{
    using Error::Error;
};

struct ToolTimeoutError : public Error
{
    using Error::Error;
};

struct TransportError : public Error
{
    using Error::Error;
};

} // namespace fastmcpp
