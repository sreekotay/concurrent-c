#ifndef CC_MACRO_RECOGNIZER_H
#define CC_MACRO_RECOGNIZER_H

struct TCCState;

/* M5.5: post-CPP token-stream recognition for macro-generated CC syntax. */
int cc_macro_recognizer_type_position(void* userdata, struct TCCState* s);
int cc_macro_recognizer_postfix(void* userdata, struct TCCState* s);

void cc_macro_recognizer_register(struct TCCState* s);

#endif
