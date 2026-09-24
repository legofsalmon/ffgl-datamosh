#pragma once

#include <map>
#include <string>
#include <vector>

namespace datamosh::licence::json {

/// One value from a JSON object. Strings are decoded; anything else (a number,
/// true/false/null, a nested object or array) is kept as its raw source text so
/// it can be read as a number or parsed again.
struct Value
{
	bool        isString = false;
	std::string text;
};

/// The top-level members of a JSON object, or empty if `source` is not one.
///
/// Deliberately small, and deliberately a real parser rather than a substring
/// search. The service's error `message` is prose and can contain quotes,
/// escaped characters and non-ASCII punctuation, and that message is the one
/// thing the operator is supposed to read; a find-the-next-quote reader would
/// cut it off at the first apostrophe.
std::map< std::string, Value > ParseObject( const std::string& source );

/// The raw source text of each element of a JSON array.
std::vector< std::string > ParseArray( const std::string& source );

/// A JSON string literal, quotes included.
std::string Quote( const std::string& text );

}  // namespace datamosh::licence::json
