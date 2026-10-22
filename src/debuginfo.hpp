#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <amx/amx.h>

namespace tracer {

/**
 * @brief      The functions, variables, lines and files of a script compiled with debug information (`-d2` or `-d3`).
 */
class DebugInfo {
public:
	enum Ident : char {
		IdentVariable = 1,
		IdentReference = 2,
		IdentArray = 3,
		IdentReferenceArray = 4,
		IdentFunction = 9,
		IdentVariableArguments = 11
	};

	enum Scope : char {
		ScopeGlobal = 0,
		ScopeLocal = 1,
		ScopeStatic = 2
	};

	struct Dimension {
		int16_t tag;
		ucell size;
	};

	struct Symbol {
		std::string name;
		cell address;
		int16_t tag;
		ucell codeStart;
		ucell codeEnd;
		char ident;
		char scope;
		std::vector<Dimension> dimensions;
	};

	struct Function {
		std::string name;
		ucell start;
		ucell end;
		int16_t tag;
		std::vector<int> parameters;
	};

	/**
	 * @brief      Read the debug information that follows the AMX image in a .amx file.
	 *
	 * @param      path  Path of the .amx file.
	 *
	 * @return     - `true` the file has debug information with at least one function
	 *             - `false` the file could not be read or has none
	 */
	bool load(const std::string& path);

	/**
	 * @brief      The function whose code holds an address.
	 *
	 * @param      address  Address in the code section.
	 *
	 * @return     - the index of the function in `functions()`
	 *             - `-1` no function holds the address
	 */
	int functionAt(ucell address) const;

	/**
	 * @brief      The function that starts at an address.
	 *
	 * @param      address  Address in the code section.
	 *
	 * @return     - the index of the function in `functions()`
	 *             - `-1` no function starts there
	 */
	int functionStartingAt(ucell address) const;

	/**
	 * @brief      The source line of an address, counted from 1.
	 *
	 * @param      address  Address in the code section.
	 *
	 * @return     - the line
	 *             - `0` the address comes before every line
	 */
	long lineAt(ucell address) const;

	/**
	 * @brief      The code of the statement that holds an address.
	 *
	 * @param      address  Address in the code section.
	 * @param      start    Receives the address of the statement.
	 * @param      end      Receives the address of the statement after it, or the end of the code.
	 *
	 * @return     - `true` a statement holds the address
	 *             - `false` the address comes before every statement
	 */
	bool statementAt(ucell address, ucell& start, ucell& end) const;

	/**
	 * @brief      The source file of an address.
	 *
	 * @param      address  Address in the code section.
	 *
	 * @return     - the path the compiler gave the file
	 *             - an empty string, when the address comes before every file
	 */
	const std::string& fileAt(ucell address) const;

	/**
	 * @brief      The name of a tag.
	 *
	 * @param      tag   Tag of a symbol.
	 *
	 * @return     - the name
	 *             - an empty string, for the untagged `_` and for an unknown tag
	 */
	const std::string& tagName(int16_t tag) const;

	/**
	 * @brief      A variable found by its address.
	 *
	 * @param      address   Address of the variable, relative to the frame for a local.
	 * @param      local     Whether the address is relative to the frame.
	 * @param      position  Address in the code section where the variable is used.
	 *
	 * @return     - the symbol
	 *             - `nullptr` no variable lives there
	 */
	const Symbol* variableAt(cell address, bool local, ucell position) const;

	const std::vector<Function>& functions() const { return functions_; }
	const std::vector<Symbol>& symbols() const { return symbols_; }

private:
	struct File {
		ucell address;
		std::string name;
	};

	struct Line {
		ucell address;
		int32_t line;
	};

	std::vector<Function> functions_;
	std::vector<Symbol> symbols_;
	std::vector<Line> lines_;
	std::vector<File> files_;
	std::unordered_map<int16_t, std::string> tags_;
	ucell codeSize_ = 0;
};

}
