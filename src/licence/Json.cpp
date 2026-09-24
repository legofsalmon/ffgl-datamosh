#include "Json.h"

#include <cctype>
#include <cstdint>
#include <cstdio>

namespace datamosh::licence::json {

namespace {

class Reader
{
public:
	explicit Reader( const std::string& source ) :
		text( source )
	{
	}

	void SkipSpace()
	{
		while( at < text.size() && std::isspace( static_cast< unsigned char >( text[ at ] ) ) )
			++at;
	}

	bool Consume( char expected )
	{
		SkipSpace();
		if( at < text.size() && text[ at ] == expected )
		{
			++at;
			return true;
		}
		return false;
	}

	bool Peek( char expected )
	{
		SkipSpace();
		return at < text.size() && text[ at ] == expected;
	}

	bool AtEnd()
	{
		SkipSpace();
		return at >= text.size();
	}

	/// A string literal, decoded. The opening quote must be next.
	bool ReadString( std::string& out )
	{
		if( !Consume( '"' ) )
			return false;
		out.clear();
		while( at < text.size() )
		{
			const char c = text[ at++ ];
			if( c == '"' )
				return true;
			if( c != '\\' )
			{
				out.push_back( c );
				continue;
			}
			if( at >= text.size() )
				return false;
			const char escape = text[ at++ ];
			switch( escape )
			{
			case '"':  out.push_back( '"' ); break;
			case '\\': out.push_back( '\\' ); break;
			case '/':  out.push_back( '/' ); break;
			case 'b':  out.push_back( '\b' ); break;
			case 'f':  out.push_back( '\f' ); break;
			case 'n':  out.push_back( '\n' ); break;
			case 'r':  out.push_back( '\r' ); break;
			case 't':  out.push_back( '\t' ); break;
			case 'u':
			{
				std::uint32_t code = 0;
				if( !ReadHex4( code ) )
					return false;
				// A surrogate pair is two escapes that make one character.
				if( code >= 0xD800 && code <= 0xDBFF && at + 1 < text.size() && text[ at ] == '\\' &&
				    text[ at + 1 ] == 'u' )
				{
					at += 2;
					std::uint32_t low = 0;
					if( !ReadHex4( low ) )
						return false;
					if( low >= 0xDC00 && low <= 0xDFFF )
						code = 0x10000 + ( ( code - 0xD800 ) << 10 ) + ( low - 0xDC00 );
				}
				AppendUtf8( out, code );
				break;
			}
			default:
				return false;
			}
		}
		return false;
	}

	/// Skips one value of any kind and returns its source text.
	bool ReadRaw( std::string& out )
	{
		SkipSpace();
		const size_t start = at;
		if( !SkipValue() )
			return false;
		out = text.substr( start, at - start );
		return true;
	}

	size_t at = 0;

private:
	bool ReadHex4( std::uint32_t& out )
	{
		if( at + 4 > text.size() )
			return false;
		out = 0;
		for( int i = 0; i < 4; ++i )
		{
			const char c = text[ at++ ];
			out <<= 4;
			if( c >= '0' && c <= '9' )
				out |= static_cast< std::uint32_t >( c - '0' );
			else if( c >= 'a' && c <= 'f' )
				out |= static_cast< std::uint32_t >( c - 'a' + 10 );
			else if( c >= 'A' && c <= 'F' )
				out |= static_cast< std::uint32_t >( c - 'A' + 10 );
			else
				return false;
		}
		return true;
	}

	static void AppendUtf8( std::string& out, std::uint32_t code )
	{
		if( code < 0x80 )
			out.push_back( static_cast< char >( code ) );
		else if( code < 0x800 )
		{
			out.push_back( static_cast< char >( 0xC0 | ( code >> 6 ) ) );
			out.push_back( static_cast< char >( 0x80 | ( code & 0x3F ) ) );
		}
		else if( code < 0x10000 )
		{
			out.push_back( static_cast< char >( 0xE0 | ( code >> 12 ) ) );
			out.push_back( static_cast< char >( 0x80 | ( ( code >> 6 ) & 0x3F ) ) );
			out.push_back( static_cast< char >( 0x80 | ( code & 0x3F ) ) );
		}
		else
		{
			out.push_back( static_cast< char >( 0xF0 | ( code >> 18 ) ) );
			out.push_back( static_cast< char >( 0x80 | ( ( code >> 12 ) & 0x3F ) ) );
			out.push_back( static_cast< char >( 0x80 | ( ( code >> 6 ) & 0x3F ) ) );
			out.push_back( static_cast< char >( 0x80 | ( code & 0x3F ) ) );
		}
	}

	bool SkipValue()
	{
		SkipSpace();
		if( at >= text.size() )
			return false;
		const char c = text[ at ];
		if( c == '"' )
		{
			std::string ignored;
			return ReadString( ignored );
		}
		if( c == '{' || c == '[' )
		{
			const char close = ( c == '{' ) ? '}' : ']';
			++at;
			if( Consume( close ) )
				return true;
			for( ;; )
			{
				if( c == '{' )
				{
					std::string key;
					if( !ReadString( key ) || !Consume( ':' ) )
						return false;
				}
				if( !SkipValue() )
					return false;
				if( Consume( ',' ) )
					continue;
				return Consume( close );
			}
		}
		// A number or a literal: everything up to the next delimiter.
		const size_t start = at;
		while( at < text.size() && text[ at ] != ',' && text[ at ] != '}' && text[ at ] != ']' &&
		       !std::isspace( static_cast< unsigned char >( text[ at ] ) ) )
			++at;
		return at > start;
	}

	const std::string& text;
};

}  // namespace

std::map< std::string, Value > ParseObject( const std::string& source )
{
	std::map< std::string, Value > members;
	Reader reader( source );
	if( !reader.Consume( '{' ) )
		return {};
	if( reader.Consume( '}' ) )
		return members;

	for( ;; )
	{
		std::string key;
		if( !reader.ReadString( key ) || !reader.Consume( ':' ) )
			return {};

		Value value;
		if( reader.Peek( '"' ) )
		{
			value.isString = true;
			if( !reader.ReadString( value.text ) )
				return {};
		}
		else if( !reader.ReadRaw( value.text ) )
			return {};

		members[ key ] = std::move( value );

		if( reader.Consume( ',' ) )
			continue;
		if( reader.Consume( '}' ) && reader.AtEnd() )
			return members;
		return {};
	}
}

std::vector< std::string > ParseArray( const std::string& source )
{
	std::vector< std::string > items;
	Reader reader( source );
	if( !reader.Consume( '[' ) )
		return {};
	if( reader.Consume( ']' ) )
		return items;
	for( ;; )
	{
		std::string item;
		if( !reader.ReadRaw( item ) )
			return {};
		items.push_back( std::move( item ) );
		if( reader.Consume( ',' ) )
			continue;
		if( reader.Consume( ']' ) )
			return items;
		return {};
	}
}

std::string Quote( const std::string& text )
{
	std::string out = "\"";
	for( const char c : text )
	{
		switch( c )
		{
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if( static_cast< unsigned char >( c ) < 0x20 )
			{
				char buffer[ 8 ];
				std::snprintf( buffer, sizeof( buffer ), "\\u%04x", static_cast< unsigned char >( c ) );
				out += buffer;
			}
			else
				out.push_back( c );
		}
	}
	out.push_back( '"' );
	return out;
}

}  // namespace datamosh::licence::json
