#pragma once

#ifdef _WIN32

#include <cassert>
#include <memory>
#include <objbase.h>
#include <utility>

namespace wil
{

template <typename close_fn_t, close_fn_t close_fn, bool default_value = true>
class unique_call
{
public:
  unique_call() = default;

  explicit unique_call(bool call) noexcept : m_call(call) {}

  unique_call(unique_call&& other) noexcept
  {
    m_call = other.m_call;
    other.m_call = false;
  }

  unique_call& operator=(unique_call&& other) noexcept
  {
    if (this != std::addressof(other))
    {
      reset();
      m_call = other.m_call;
      other.m_call = false;
    }
    return *this;
  }

  ~unique_call() noexcept { reset(); }

  //! Assigns a new ptr and token
  void reset() noexcept
  {
    auto call = m_call;
    m_call = false;
    if (call)
    {
      close_fn();
    }
  }

  //! Exchanges values with raii class
  void swap(unique_call& other) noexcept { std::swap(m_call, other.m_call); }

  //! Make the interface call that was expected of this class
  void activate() noexcept { m_call = true; }

  //! Do not make the interface call that was expected of this class
  void release() noexcept { m_call = false; }

  //! Returns true if the call that was expected is still outstanding
  [[nodiscard]] explicit operator bool() const noexcept { return m_call; }

  unique_call(const unique_call&) = delete;
  unique_call& operator=(const unique_call&) = delete;

private:
  bool m_call = default_value;
};

using unique_couninitialize_call = unique_call<decltype(&::CoUninitialize), ::CoUninitialize>;
}  // namespace wil

namespace wil
{
class last_error_context
{
#ifndef WIL_KERNEL_MODE
  bool m_dismissed = false;
  DWORD m_error = 0;

public:
  last_error_context() noexcept : last_error_context(::GetLastError()) {}

  explicit last_error_context(DWORD error) noexcept : m_error(error) {}

  last_error_context(last_error_context&& other) noexcept { operator=(std::move(other)); }

  last_error_context& operator=(last_error_context&& other) noexcept
  {
    m_dismissed = std::exchange(other.m_dismissed, true);
    m_error = other.m_error;

    return *this;
  }

  ~last_error_context() noexcept
  {
    if (!m_dismissed)
    {
      ::SetLastError(m_error);
    }
  }

  //! last_error_context doesn't own a concrete resource, so therefore
  //! it just disarms its destructor and returns void.
  void release() noexcept
  {
    assert(!m_dismissed);
    m_dismissed = true;
  }

  [[nodiscard]] auto value() const noexcept { return m_error; }
#else
public:
  void release() noexcept {}
#endif  // WIL_KERNEL_MODE
};

namespace details
{
typedef std::integral_constant<size_t, 0>
    pointer_access_all;  // get(), release(), addressof(), and '&' are available
typedef std::integral_constant<size_t, 1>
    pointer_access_noaddress;                                   // get() and release() are available
typedef std::integral_constant<size_t, 2> pointer_access_none;  // the raw pointer is not available

template <bool is_fn_ptr, typename close_fn_t, close_fn_t close_fn, typename pointer_storage_t>
struct close_invoke_helper
{
  __forceinline static void close(pointer_storage_t value) noexcept
  {
    std::invoke(close_fn, value);
  }
  static void close_reset(pointer_storage_t value) noexcept
  {
    auto preserveError = last_error_context();
    std::invoke(close_fn, value);
  }
};

template <typename close_fn_t, close_fn_t close_fn, typename pointer_storage_t>
struct close_invoke_helper<true, close_fn_t, close_fn, pointer_storage_t>
{
  __forceinline static void close(pointer_storage_t value) noexcept { close_fn(value); }
  static void close_reset(pointer_storage_t value) noexcept
  {
    auto preserveError = last_error_context();
    close_fn(value);
  }
};

template <typename close_fn_t, close_fn_t close_fn, typename pointer_storage_t>
using close_invoker = close_invoke_helper<
    std::is_pointer_v<close_fn_t> ? std::is_function_v<std::remove_pointer_t<close_fn_t>> : false,
    close_fn_t, close_fn, pointer_storage_t>;
}  // namespace details

template <typename struct_t, typename close_fn_t, close_fn_t close_fn,
          typename init_fn_t = std::nullptr_t, init_fn_t init_fn = std::nullptr_t()>
class unique_struct : public struct_t
{
  using closer = details::close_invoker<close_fn_t, close_fn, struct_t*>;

public:
  //! Initializes the managed struct using the user-provided initialization function, or ZeroMemory
  //! if no function is specified
  unique_struct() { call_init(use_default_init_fn()); }

  //! Takes ownership of the struct by doing a shallow copy. Must explicitly be type struct_t
  explicit unique_struct(const struct_t& other) noexcept : struct_t(other) {}

  //! Initializes the managed struct by taking the ownership of the other managed struct
  //! Then resets the other managed struct by calling the custom close function
  unique_struct(unique_struct&& other) noexcept : struct_t(other.release()) {}

  //! Resets this managed struct by calling the custom close function and takes ownership of the
  //! other managed struct Then resets the other managed struct by calling the custom close function
  unique_struct& operator=(unique_struct&& other) noexcept
  {
    if (this != std::addressof(other))
    {
      reset(other.release());
    }
    return *this;
  }

  //! Calls the custom close function
  ~unique_struct() noexcept { closer::close(this); }

  void reset(const unique_struct&) = delete;

  //! Resets this managed struct by calling the custom close function and begins management of the
  //! other struct
  void reset(const struct_t& other) noexcept
  {
    closer::close_reset(this);
    struct_t::operator=(other);
  }

  //! Resets this managed struct by calling the custom close function
  //! Then initializes this managed struct using the user-provided initialization function, or
  //! ZeroMemory if no function is specified
  void reset() noexcept
  {
    closer::close(this);
    call_init(use_default_init_fn());
  }

  void swap(struct_t&) = delete;

  //! Swaps the managed structs
  void swap(unique_struct& other) noexcept
  {
    struct_t self(*this);
    struct_t::operator=(other);
    *(other.addressof()) = self;
  }

  //! Returns the managed struct
  //! Then initializes this managed struct using the user-provided initialization function, or
  //! ZeroMemory if no function is specified
  struct_t release() noexcept
  {
    struct_t value(*this);
    call_init(use_default_init_fn());
    return value;
  }

  //! Returns address of the managed struct
  struct_t* addressof() noexcept { return this; }

  //! Resets this managed struct by calling the custom close function
  //! Then initializes this managed struct using the user-provided initialization function, or
  //! ZeroMemory if no function is specified. Returns address of the managed struct
  struct_t* reset_and_addressof() noexcept
  {
    reset();
    return this;
  }

  unique_struct(const unique_struct&) = delete;
  unique_struct& operator=(const unique_struct&) = delete;
  unique_struct& operator=(const struct_t&) = delete;

private:
  typedef typename std::is_same<init_fn_t, std::nullptr_t>::type use_default_init_fn;

  void call_init(std::true_type)
  {
    // Suppress '-Wnontrivial-memcall' with 'static_cast'
    RtlZeroMemory(static_cast<void*>(this), sizeof(*this));
  }

  void call_init(std::false_type) { init_fn(this); }
};

using unique_prop_variant =
    wil::unique_struct<PROPVARIANT, decltype(&::PropVariantClear), ::PropVariantClear,
                       decltype(&::PropVariantInit), ::PropVariantInit>;
}  // namespace wil

namespace wil
{
struct unique_cotaskmem_string
{
  PWSTR ptr{nullptr};

  unique_cotaskmem_string() noexcept = default;

  ~unique_cotaskmem_string()
  {
    if (ptr)
    {
      CoTaskMemFree(ptr);
    }
  }

  unique_cotaskmem_string(const unique_cotaskmem_string&) = delete;
  unique_cotaskmem_string& operator=(const unique_cotaskmem_string&) = delete;

  unique_cotaskmem_string(unique_cotaskmem_string&& other) noexcept : ptr(other.ptr)
  {
    other.ptr = nullptr;
  }

  unique_cotaskmem_string& operator=(unique_cotaskmem_string&& other) noexcept
  {
    if (this != &other)
    {
      if (ptr)
      {
        CoTaskMemFree(ptr);
      }
      ptr = other.ptr;
      other.ptr = nullptr;
    }
    return *this;
  }

  PWSTR* put() { return &ptr; }
  PWSTR get() const { return ptr; }
};

struct unique_hkey
{
  HKEY h{nullptr};

  unique_hkey() noexcept = default;

  ~unique_hkey()
  {
    if (h)
    {
      RegCloseKey(h);
    }
  }

  unique_hkey(const unique_hkey&) = delete;
  unique_hkey& operator=(const unique_hkey&) = delete;

  unique_hkey(unique_hkey&& other) noexcept : h(other.h) { other.h = nullptr; }

  unique_hkey& operator=(unique_hkey&& other) noexcept
  {
    if (this != &other)
    {
      if (h)
      {
        RegCloseKey(h);
      }
      h = other.h;
      other.h = nullptr;
    }
    return *this;
  }

  HKEY* put() { return &h; }
  HKEY get() const { return h; }
};
}  // namespace wil

#endif
