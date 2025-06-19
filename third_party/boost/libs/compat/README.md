# Boost.Compat

Boost.Compat is a repository of C++11 implementations
of standard components added in later C++ standards. Its
target audience is Boost library authors whose libraries
support a lower C++ standard, but wish to utilize a component
added in a subsequent one.

The criteria for inclusion in Boost.Compat are as follows:

* The implementation should be relatively simple and
  header-only.
* The component must implement the standard functionality
  exactly, without deviations or extensions. This allows
  (but does not require) the implementation to be a simple
  `using` declaration in case the standard component is
  available.
* The component must not depend on any Boost libraries
  except Boost.Config, Boost.Assert, or Boost.ThrowException.
* The component must not be a vocabulary type, visible in
  the library APIs. The user should never see a `boost::compat`
  type; the use of Compat types should be confined to library
  implementations.
