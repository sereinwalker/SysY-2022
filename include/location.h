#ifndef LOCATION_H
#define LOCATION_H

/**
 * @file location.h
 * @brief Defines the structure for representing a location within the source code.
 *
 * This header provides the `SourceLocation` structure, a fundamental component
 * used throughout the compiler to track the precise position of various language
 * constructs. By associating every token and AST node with its source location,
 * the compiler can produce highly informative and user-friendly error messages,
 * pointing directly to the origin of an issue in the user's code.
 */

/**
 * @struct SourceLocation
 * @brief Represents a region in the source code, defined by its start and end points.
 *
 * This structure is versatile enough to represent a single character, a multi-character
 * token (like an identifier), or a complete syntactic construct (like a function
 * definition spanning multiple lines). It is heavily used by the lexical analyzer (Flex),
 * the parser (Bison), and the error reporting module.
 *
 * All line and column numbers are 1-based, which is the standard convention for
 * text editors and user-facing diagnostics.
 */
typedef struct SourceLocation {
    /**
     * @brief The line number where this location begins. (1-based)
     */
    int first_line;

    /**
     * @brief The column number on the `first_line` where this location begins. (1-based)
     */
    int first_column;

    /**
     * @brief The line number where this location ends. (1-based)
     * For single-line constructs, this will be the same as `first_line`.
     */
    int last_line;

    /**
     * @brief The column number on the `last_line` where this location ends. (1-based)
     */
    int last_column;
} SourceLocation;

#endif // LOCATION_H