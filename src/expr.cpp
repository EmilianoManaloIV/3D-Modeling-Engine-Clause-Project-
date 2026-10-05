#include "expr.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace expr {
namespace {

// Recursive descent: sum := product (('+'|'-') product)*
//                    product := unary (('*'|'/') unary)*
//                    unary := ('-'|'+') unary | power
//                    power := atom ('^' unary)?
//                    atom := number | 'pi' | '(' sum ')'
struct Parser {
    const char* p;
    bool ok = true;

    void skip() {
        while (*p == ' ' || *p == '\t') ++p;
    }
    double sum() {
        double v = product();
        for (;;) {
            skip();
            if (*p == '+') {
                ++p;
                v += product();
            } else if (*p == '-') {
                ++p;
                v -= product();
            } else {
                return v;
            }
        }
    }
    double product() {
        double v = unary();
        for (;;) {
            skip();
            if (*p == '*') {
                ++p;
                v *= unary();
            } else if (*p == '/') {
                ++p;
                double d = unary();
                if (d == 0.0) ok = false;
                else v /= d;
            } else {
                return v;
            }
        }
    }
    // Unary minus binds looser than '^' (-2^2 = -4), as in math and Python.
    double unary() {
        skip();
        if (*p == '-') {
            ++p;
            return -unary();
        }
        if (*p == '+') {
            ++p;
            return unary();
        }
        return power();
    }
    double power() {
        double b = atom();
        skip();
        if (*p == '^') {
            ++p;
            return std::pow(b, unary());  // right associative; allows 2^-1
        }
        return b;
    }
    double atom() {
        skip();
        if (*p == '(') {
            ++p;
            double v = sum();
            skip();
            if (*p == ')') ++p;
            else ok = false;
            return v;
        }
        if ((p[0] == 'p' || p[0] == 'P') && (p[1] == 'i' || p[1] == 'I')) {
            p += 2;
            return 3.14159265358979323846;
        }
        if (std::isdigit((unsigned char)*p) || *p == '.') {
            char* end = nullptr;
            double v = std::strtod(p, &end);
            if (end == p) {
                ok = false;
                return 0;
            }
            p = end;
            return v;
        }
        ok = false;
        return 0;
    }
};

}  // namespace

bool evaluate(const std::string& text, double current, double& out) {
    size_t s = 0;
    while (s < text.size() && std::isspace((unsigned char)text[s])) ++s;
    std::string t = text.substr(s);
    if (t.empty()) return false;
    char rel = 0;
    if (t.size() >= 2 && t[1] == '=' && (t[0] == '+' || t[0] == '-' || t[0] == '*' || t[0] == '/')) {
        rel = t[0];
        t = t.substr(2);
    }
    Parser ps{t.c_str()};
    double v = ps.sum();
    ps.skip();
    if (!ps.ok || *ps.p != '\0' || !std::isfinite(v)) return false;
    switch (rel) {
        case '+': v = current + v; break;
        case '-': v = current - v; break;
        case '*': v = current * v; break;
        case '/':
            if (v == 0.0) return false;
            v = current / v;
            break;
        default: break;
    }
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

}  // namespace expr
