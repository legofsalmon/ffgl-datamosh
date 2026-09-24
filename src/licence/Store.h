#pragma once

// The per-user licence folder:
//
//   macOS    ~/Library/Application Support/LeTissier/Datamosh/
//   Windows  %APPDATA%\LeTissier\Datamosh
//   Linux    $XDG_CONFIG_HOME/LeTissier/Datamosh/  (tests only; not a Resolume platform)
//
// Deliberately outside anything Resolume saves. A composition is a file people
// send each other, and a licence key inside one would be a licence key given
// away.
//
// Both plugin binaries read and write the same folder. They are separate
// libraries with separate copies of every static, so this folder IS the
// process-wide state: when the effect activates, the mixer's worker sees the
// token file change and picks it up.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace datamosh::licence {

class Store
{
public:
	static constexpr const char* TOKEN_FILE      = "licence.token";
	static constexpr const char* KEY_FILE        = "licence.key";
	static constexpr const char* CHECK_IN_FILE   = "last-check-in";
	static constexpr const char* README_FILE     = "README.txt";
	/// Where an offline-activation token can be saved by hand. Imported, checked
	/// and removed by the worker — the fallback for a host whose text field will
	/// not take a few hundred pasted characters.
	static constexpr const char* DROP_FILE       = "activation-token.txt";
	/// Present while the service has revoked this computer's licence (a full
	/// refund ends it). Holds the service's own words, for the README. The key
	/// stays beside it, so a later check-in can restore a reinstated licence.
	static constexpr const char* REVOKED_FILE    = "revoked";

	explicit Store( std::filesystem::path directory );

	const std::filesystem::path& Directory() const { return directory; }

	bool Ensure() const;

	std::optional< std::string > ReadToken() const { return Read( TOKEN_FILE ); }
	std::optional< std::string > ReadKey() const { return Read( KEY_FILE ); }
	std::optional< std::string > ReadDropFile() const { return Read( DROP_FILE ); }

	bool WriteToken( const std::string& token ) const { return Write( TOKEN_FILE, token ); }
	bool WriteKey( const std::string& key ) const { return Write( KEY_FILE, key ); }
	bool WriteReadme( const std::string& text ) const { return Write( README_FILE, text ); }
	bool RemoveDropFile() const;

	/// Removes the token only, keeping the key: what a revocation does.
	void RemoveToken() const;

	std::optional< std::string > ReadRevoked() const { return Read( REVOKED_FILE ); }
	bool WriteRevoked( const std::string& message ) const { return Write( REVOKED_FILE, message ); }
	void ClearRevoked() const;

	/// Removes the token, the key and any revocation. The folder, README and
	/// check-in record stay.
	void Forget() const;

	std::int64_t LastCheckIn() const;
	void         RecordCheckIn( std::int64_t when ) const;

	/// Changes whenever a file that decides the licence changes — including
	/// when the other plugin binary writes one.
	std::string Stamp() const;

private:
	std::optional< std::string > Read( const char* name ) const;
	/// Written to a temporary name and renamed over the target, so the other
	/// binary can never read half a token.
	bool Write( const char* name, const std::string& contents ) const;

	std::filesystem::path directory;
};

}  // namespace datamosh::licence
