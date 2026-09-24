#include "Wire.h"

#include "Json.h"

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
