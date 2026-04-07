#ifndef SQL_LEXER_H
#define SQL_LEXER_H

#include <cstddef>
#include <string_view>

enum class TokenType {
    KEYWORD,
    IDENTIFIER,
    INTEGER_LITERAL,
    STRING_LITERAL,
    LPAREN,
    RPAREN,
    COMMA,
    SEMICOLON,
    DOT,
    STAR,
    EQ,
    LT,
    GT,
    LE,
    GE,
    END_OF_INPUT,
    UNKNOWN
};

struct Token {
    TokenType type;
    std::string_view text;
};

class Lexer {
public:
    explicit Lexer(std::string_view input) : _input(input) {}

    Token next() {
        if (_has_peek) {
            _has_peek = false;
            return _peek_tok;
        }
        return next_raw();
    }

    Token peek() {
        if (!_has_peek) {
            _peek_tok = next_raw();
            _has_peek = true;
        }
        return _peek_tok;
    }

    void consume() {
        (void)next();
    }

private:
    static bool is_ident_start(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    }

    static bool is_ident_char(char c) {
        return is_ident_start(c) || (c >= '0' && c <= '9');
    }

    static bool is_keyword(std::string_view t) {
        auto ieq = [](std::string_view a, const char* b) {
            size_t n = a.size();
            size_t i = 0;
            for (; b[i] != '\0'; ++i) {
                if (i >= n) {
                    return false;
                }
                char ca = a[i];
                char cb = b[i];
                if (ca >= 'a' && ca <= 'z') {
                    ca = static_cast<char>(ca - 'a' + 'A');
                }
                if (cb >= 'a' && cb <= 'z') {
                    cb = static_cast<char>(cb - 'a' + 'A');
                }
                if (ca != cb) {
                    return false;
                }
            }
            return i == n;
        };

         return ieq(t, "CREATE") || ieq(t, "TABLE") || ieq(t, "DROP") || ieq(t, "IF") ||
               ieq(t, "EXISTS") || ieq(t, "INSERT") || ieq(t, "INTO") || ieq(t, "VALUES") ||
               ieq(t, "SELECT") || ieq(t, "FROM") || ieq(t, "WHERE") || ieq(t, "INNER") ||
               ieq(t, "JOIN") || ieq(t, "ON") || ieq(t, "AND") || ieq(t, "OR") ||
             ieq(t, "NOT") || ieq(t, "NULL") || ieq(t, "PRIMARY") || ieq(t, "KEY") ||
             ieq(t, "DECIMAL") || ieq(t, "VARCHAR") || ieq(t, "TEXT") || ieq(t, "INT") ||
             ieq(t, "DATETIME") || ieq(t, "FLOAT") || ieq(t, "REAL") || ieq(t, "COUNT") ||
             ieq(t, "AS");
    }

    Token next_raw() {
        while (_pos < _input.size() &&
               (_input[_pos] == ' ' || _input[_pos] == '\t' || _input[_pos] == '\n' || _input[_pos] == '\r')) {
            ++_pos;
        }

        if (_pos >= _input.size()) {
            return {TokenType::END_OF_INPUT, std::string_view()};
        }

        char c = _input[_pos];
        size_t start = _pos;

        switch (c) {
            case '(':
                ++_pos;
                return {TokenType::LPAREN, _input.substr(start, 1)};
            case ')':
                ++_pos;
                return {TokenType::RPAREN, _input.substr(start, 1)};
            case ',':
                ++_pos;
                return {TokenType::COMMA, _input.substr(start, 1)};
            case ';':
                ++_pos;
                return {TokenType::SEMICOLON, _input.substr(start, 1)};
            case '.':
                ++_pos;
                return {TokenType::DOT, _input.substr(start, 1)};
            case '*':
                ++_pos;
                return {TokenType::STAR, _input.substr(start, 1)};
            case '=':
                ++_pos;
                return {TokenType::EQ, _input.substr(start, 1)};
            case '<':
                if (_pos + 1 < _input.size() && _input[_pos + 1] == '=') {
                    _pos += 2;
                    return {TokenType::LE, _input.substr(start, 2)};
                }
                ++_pos;
                return {TokenType::LT, _input.substr(start, 1)};
            case '>':
                if (_pos + 1 < _input.size() && _input[_pos + 1] == '=') {
                    _pos += 2;
                    return {TokenType::GE, _input.substr(start, 2)};
                }
                ++_pos;
                return {TokenType::GT, _input.substr(start, 1)};
            case '\'': {
                ++_pos;
                size_t s = _pos;
                while (_pos < _input.size() && _input[_pos] != '\'') {
                    ++_pos;
                }
                std::string_view text;
                if (_pos <= _input.size()) {
                    text = _input.substr(s, _pos - s);
                }
                if (_pos < _input.size() && _input[_pos] == '\'') {
                    ++_pos;
                }
                return {TokenType::STRING_LITERAL, text};
            }
            default:
                break;
        }

        if (c >= '0' && c <= '9') {
            ++_pos;
            bool dot_seen = false;
            while (_pos < _input.size()) {
                char d = _input[_pos];
                if (d >= '0' && d <= '9') {
                    ++_pos;
                    continue;
                }
                if (d == '.' && !dot_seen) {
                    dot_seen = true;
                    ++_pos;
                    continue;
                }
                break;
            }
            return {TokenType::INTEGER_LITERAL, _input.substr(start, _pos - start)};
        }

        if ((c == '-' || c == '+') &&
            (_pos + 1 < _input.size()) &&
            (_input[_pos + 1] >= '0' && _input[_pos + 1] <= '9')) {
            ++_pos;
            bool dot_seen = false;
            while (_pos < _input.size()) {
                char d = _input[_pos];
                if (d >= '0' && d <= '9') {
                    ++_pos;
                    continue;
                }
                if (d == '.' && !dot_seen) {
                    dot_seen = true;
                    ++_pos;
                    continue;
                }
                break;
            }
            return {TokenType::INTEGER_LITERAL, _input.substr(start, _pos - start)};
        }

        if (is_ident_start(c)) {
            ++_pos;
            while (_pos < _input.size() && is_ident_char(_input[_pos])) {
                ++_pos;
            }
            std::string_view t = _input.substr(start, _pos - start);
            if (is_keyword(t)) {
                return {TokenType::KEYWORD, t};
            }
            return {TokenType::IDENTIFIER, t};
        }

        ++_pos;
        return {TokenType::UNKNOWN, _input.substr(start, 1)};
    }

    std::string_view _input;
    size_t _pos = 0;
    bool _has_peek = false;
    Token _peek_tok{TokenType::END_OF_INPUT, std::string_view()};
};

#endif
