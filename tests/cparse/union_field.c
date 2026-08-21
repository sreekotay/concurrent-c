typedef struct UnionField {
    unsigned long long meta;
    union {
        const char* bytes;
        int i64;
    };
    int extra;
} UnionField;
