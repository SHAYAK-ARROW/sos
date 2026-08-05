#include "ctype.h"

int isalpha(int c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}

int isdigit(int c) {
    return (c >= '0' && c <= '9');
}

int isspace(int c) {
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
}

int toupper(int c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

int tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}
