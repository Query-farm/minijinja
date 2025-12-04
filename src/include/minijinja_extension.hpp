#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Extension version - update this single location when releasing new versions
#define MINIJINJA_EXTENSION_VERSION "2025101901"

class MinijinjaExtension : public Extension {
public:
	void Load(ExtensionLoader &db) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
