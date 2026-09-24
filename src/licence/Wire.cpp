#include "Wire.h"

#include "Json.h"
#include "Licence.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace datamosh::licence {

namespace {

std::string Field( const std::map< std::string, json::Value >& object, const char* name )
{
	const auto found = object.find( name );
	if( found == object.end() )
		return {};
	return found->second.isString ? found->second.text : std::string();
}

}  // namespace

std::string ServiceUrl()
{
	const char* overridden = std::getenv( "LETISSIER_API" );
	if( overridden && ParseServiceUrl( overridden ).valid )
	{
		std::string url = overridden;
		while( !url.empty() && url.back() == '/' )
			url.pop_back();
		return url;
	}
	return SERVICE_URL;
}

ServiceAddress ParseServiceUrl( const std::string& url )
{
	ServiceAddress address;
	std::string    rest;
	if( url.rfind( "https://", 0 ) == 0 )
	{
		address.secure = true;
		address.port   = 443;
		rest           = url.substr( 8 );
	}
	else if( url.rfind( "http://", 0 ) == 0 )
	{
		address.secure = false;
		address.port   = 80;
		rest           = url.substr( 7 );
	}
	else
		return address;

	const size_t slash = rest.find( '/' );
	std::string  authority = rest.substr( 0, slash );
	address.basePath       = slash == std::string::npos ? std::string() : rest.substr( slash );
	while( !address.basePath.empty() && address.basePath.back() == '/' )
		address.basePath.pop_back();
	// No credentials, query or fragment in a base URL.
	if( authority.empty() || authority.find( '@' ) != std::string::npos ||
	    address.basePath.find_first_of( "?#" ) != std::string::npos )
		return address;

	const size_t colon = authority.rfind( ':' );
	if( colon != std::string::npos )
	{
		const std::string digits = authority.substr( colon + 1 );
		if( digits.empty() || digits.size() > 5 ||
		    !std::all_of( digits.begin(), digits.end(), []( char c ) { return std::isdigit( static_cast< unsigned char >( c ) ) != 0; } ) )
			return address;
		address.port = std::atoi( digits.c_str() );
		if( address.port <= 0 || address.port > 65535 )
			return address;
		authority.erase( colon );
	}
	if( authority.empty() )
		return address;
	address.host  = authority;
	address.valid = true;
	return address;
}

Reply ParseReply( const HttpResponse& response )
{
	Reply reply;
	reply.reached        = response.reached;
	reply.http           = response.status;
	reply.transportError = response.error;
	if( !response.reached )
		return reply;

	const auto object = json::ParseObject( response.body );
	const auto ok     = object.find( "ok" );
	reply.ok          = ok != object.end() && !ok->second.isString && ok->second.text == "true" &&
	           response.status >= 200 && response.status < 300;

	reply.token            = Field( object, "token" );
	reply.machine          = Field( object, "machine" );
	reply.key              = Field( object, "key" );
	reply.product          = Field( object, "product" );
	reply.edition          = Field( object, "edition" );
	reply.checkInBy        = Field( object, "checkInBy" );
	reply.maintenanceUntil = Field( object, "maintenanceUntil" );
	reply.expiresAt        = Field( object, "expiresAt" );
	reply.reason           = Field( object, "reason" );
	reply.message          = Field( object, "message" );

	if( !reply.ok && reply.message.empty() )
	{
		// A proxy's HTML error page, a 502 from a CDN: something answered, but
		// not the service. Say which status so the README is some use.
		reply.message = "The licence service could not be reached properly (HTTP " +
		                std::to_string( response.status ) + ").";
		if( reply.reason.empty() )
			reply.reason = "server_error";
	}
	return reply;
}

std::string ActivateBody( const std::string& key, const std::string& fingerprint, const std::string& label,
                          const std::string& product )
{
	std::string body = "{\"key\":" + json::Quote( key ) + ",\"machine\":" + json::Quote( fingerprint );
	if( !label.empty() )
		body += ",\"label\":" + json::Quote( label );
	body += ",\"product\":" + json::Quote( product ) + "}";
	return body;
}

std::string HeartbeatBody( const std::string& key, const std::string& fingerprint, const std::string& product )
{
	return "{\"key\":" + json::Quote( key ) + ",\"machine\":" + json::Quote( fingerprint ) +
	       ",\"product\":" + json::Quote( product ) + "}";
}

std::string DeactivateBody( const std::string& key, const std::string& fingerprint )
{
	return "{\"key\":" + json::Quote( key ) + ",\"machine\":" + json::Quote( fingerprint ) + "}";
}

std::string TrialBody( const std::string& product, const std::string& email, const std::string& fingerprint )
{
	return "{\"product\":" + json::Quote( product ) + ",\"email\":" + json::Quote( email ) +
	       ",\"machine\":" + json::Quote( fingerprint ) + "}";
}

}  // namespace datamosh::licence
