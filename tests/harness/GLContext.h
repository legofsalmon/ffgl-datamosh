#pragma once

#include <string>

namespace datamosh::test {

/// A headless OpenGL 4.1 core context.
///
/// Resolume never runs on Linux, but the tests do: this is what lets CI check
/// the estimator against known-answer inputs on every commit instead of waiting
/// for someone to open the plugin and squint at it.
class GLContext
{
public:
	GLContext() = default;
	~GLContext();

	GLContext( const GLContext& )            = delete;
	GLContext& operator=( const GLContext& ) = delete;

	/// Creates and makes the context current.
	/// \return false with `error` populated if no context could be created.
	bool Create();

	const std::string& GetError() const { return error; }
	/// Reported GL version string, for the test log.
	std::string Describe() const;

private:
	void Destroy();

	void* display = nullptr;
	void* context = nullptr;
	std::string error;
};

}  // namespace datamosh::test
