#include "Licence.h"

#include <algorithm>
#include <cctype>

#ifndef DATAMOSH_BUILD_DATE
#error "DATAMOSH_BUILD_DATE must be compiled in (src/licence/CMakeLists.txt does this)"
#endif

namespace datamosh::licence {

namespace {

std::string Trim( const std::string& text )
{
	const auto first = std::find_if_not( text.begin(), text.end(),
	                                     []( unsigned char c ) { return std::isspace( c ); } );
	const auto last = std::find_if_not( text.rbegin(), text.rend(),
	                                    []( unsigned char c ) { return std::isspace( c ); } )
	                      .base();
	return first < last ? std::string( first, last ) : std::string();
}

std::string WithoutSpace( const std::string& text )
{
	std::string out;
	for( const char c : text )
	{
		if( !std::isspace( static_cast< unsigned char >( c ) ) )
			out.push_back( c );
	}
	return out;
}

std::string Lower( std::string text )
{
	for( char& c : text )
		c = static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
	return text;
}

bool IsBase64Url( char c )
{
	return ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '-' ||
	       c == '_';
}

/// LT-XXXX-XXXX-XXXX-XXXX over the service's alphabet (after folding, only
/// digits and upper case can remain).
bool HasKeyShape( const std::string& key )
{
	if( key.size() != 22 || key.compare( 0, 3, "LT-" ) != 0 )
		return false;
	for( size_t i = 3; i < key.size(); ++i )
	{
		const bool dash = ( ( i - 3 ) % 5 ) == 4;
		const char c    = key[ i ];
		if( dash ? c != '-' : !( ( c >= '0' && c <= '9' ) || ( c >= 'A' && c <= 'Z' ) ) )
			return false;
	}
	return true;
}

/// The other products' tags, folded as the service folds them. Only for the
/// friendly "that key is for X" message — and to stop a Vizz key taking a
/// Vizz seat on this machine before the service starts refusing it.
const char* ProductForTag( const std::string& tag )
{
	if( tag == "V1ZZ" ) return "Vizz";
	if( tag == "CREW" ) return "Crewbox";
	if( tag == "11GH" ) return "Light";
	if( tag == "YEWE" ) return "Yewee";
	return nullptr;
}

}  // namespace

Restriction RestrictionFor( Status status, Policy policy, bool publicKeyConfigured )
{
	if( !publicKeyConfigured || policy == Policy::Open )
		return Restriction::None;

	switch( status )
	{
	case Status::Invalid:
	case Status::Expired:
	case Status::WrongMachine:
		return policy == Policy::Lock ? Restriction::Lock : Restriction::Watermark;

	case Status::Active:
	case Status::UpdateRequired:
	case Status::CheckInRequired:
		return Restriction::None;
	}
	return Restriction::None;
}

Mark MarkFor( Status status )
{
	switch( status )
	{
	case Status::Expired:      return Mark::TrialEnded;
	case Status::Invalid:
	case Status::WrongMachine: return Mark::Unlicensed;
	default:                   return Mark::None;
	}
}

std::int64_t BuildDate()
{
	return static_cast< std::int64_t >( DATAMOSH_BUILD_DATE );
}

const std::string& PublicKey()
{
#ifdef DATAMOSH_LICENCE_PUBLIC_KEY
	static const std::string key = DATAMOSH_LICENCE_PUBLIC_KEY;
#else
	static const std::string key = letissier::kPublicKeyHex;
#endif
	return key;
}

bool PublicKeyConfigured( const std::string& publicKeyHex )
{
	return publicKeyHex.size() == 64 &&
	       std::all_of( publicKeyHex.begin(), publicKeyHex.end(),
	                    []( unsigned char c ) { return std::isxdigit( c ) != 0; } );
}

Verdict Decide( const std::string& token, const std::string& fingerprint, std::int64_t buildDate,
                std::int64_t now, const std::string& publicKeyHex, const std::string& product )
{
	const letissier::Verdict checked = letissier::check( token, fingerprint, buildDate, now, publicKeyHex );

	Verdict verdict;
	if( !checked.has_claims )
		return verdict;

	// A licence for another of the studio's products is no licence for this
	// one, whatever else about it is in order. Before the machine check, so a
	// Vizz token from this very machine does not read as "wrong machine" and
	// send someone off to re-activate a key that can never work here.
	if( checked.claims.product != product )
		return verdict;

	verdict.status    = checked.status;
	verdict.hasClaims = true;
	verdict.claims    = checked.claims;
	return verdict;
}

std::string NormaliseKey( const std::string& typed )
{
	std::string raw = WithoutSpace( typed );
	for( char& c : raw )
		c = static_cast< char >( std::toupper( static_cast< unsigned char >( c ) ) );

	if( raw.compare( 0, 3, "LT-" ) == 0 || raw.compare( 0, 3, "1T-" ) == 0 )
		raw.erase( 0, 3 );

	// Folded in the body only: the prefix has an L of its own.
	for( char& c : raw )
	{
		if( c == 'I' || c == 'L' )
			c = '1';
		else if( c == 'O' )
			c = '0';
		else if( c == 'U' )
			c = 'V';
	}
	return "LT-" + raw;
}

Input Classify( const std::string& typed )
{
	Input input;
	const std::string trimmed = Trim( typed );
	if( trimmed.empty() )
		return input;

	const std::string command = Lower( trimmed );
	if( command == "folder" || command == "open" || command == "open folder" )
	{
		input.kind = InputKind::Folder;
		return input;
	}
	if( command == "deactivate" || command == "release" )
	{
		input.kind = InputKind::Deactivate;
		return input;
	}
	if( command == "check" || command == "check in" || command == "check-in" || command == "checkin" )
	{
		input.kind = InputKind::CheckIn;
		return input;
	}

	// An email starts a trial. Loose on purpose: the service validates it and
	// says so in its own words.
	const size_t at = trimmed.find( '@' );
	if( at != std::string::npos && at > 0 && trimmed.find( '.', at ) != std::string::npos &&
	    trimmed.find_first_of( " \t\r\n" ) == std::string::npos )
	{
		input.kind  = InputKind::Email;
		input.value = trimmed;
		return input;
	}

	// A token is two base64url segments and one dot, and far longer than a key.
	// Whitespace is dropped first: a token copied out of a web page or an email
	// often arrives wrapped.
	const std::string compact = WithoutSpace( trimmed );
	const size_t      dot     = compact.find( '.' );
	if( compact.size() >= 64 && dot != std::string::npos && compact.find( '.', dot + 1 ) == std::string::npos &&
	    std::all_of( compact.begin(), compact.end(), []( char c ) { return c == '.' || IsBase64Url( c ); } ) )
	{
		input.kind  = InputKind::Token;
		input.value = compact;
		return input;
	}

	const std::string key = NormaliseKey( trimmed );
	if( HasKeyShape( key ) )
	{
		const std::string tag     = key.substr( 3, 4 );
		const char*       product = ProductForTag( tag );
		input.kind                = product ? InputKind::ForeignKey : InputKind::Key;
		input.value               = key;
		input.product             = product ? product : "";
		return input;
	}

	input.kind = InputKind::Unrecognised;
	return input;
}

std::string DescribeStatus( Status status, bool hasToken, const Claims* claims, std::int64_t now,
                            bool publicKeyConfigured )
{
	if( !publicKeyConfigured )
		return "not checked by this build";

	switch( status )
	{
	case Status::Active:
		if( claims && claims->edition == "trial" )
		{
			const std::int64_t left = claims->exp - now;
			const std::int64_t days = std::max< std::int64_t >( 1, ( left + 86399 ) / 86400 );
			return "trial, " + std::to_string( days ) + ( days == 1 ? " day left" : " days left" );
		}
		return "active";
	case Status::UpdateRequired:  return "active; update window ended";
	case Status::CheckInRequired: return "active; check-in due";
	case Status::Expired:         return "trial ended";
	case Status::WrongMachine:    return "for another computer";
	case Status::Invalid:         return hasToken ? "not valid" : "unlicensed";
	}
	return "unlicensed";
}

}  // namespace datamosh::licence
