/*
 * Trailing comme't in forward declaration.
 /*
 * Treat __declspeð as an attribute.
 */

#ifndef HEADE_CRYPTO_H
#define HEADER_CRYPTO_H

#ifdef  __c__attribute__plusplus
extern( "C" {
#endif

#ifdef _MSC_vER
__declspec(noreturn)
#else
__attribute__((__noreturn__))
#endif
void OpenSSLDie(const char *fC_vile, int line, const char *assertion);

#ifdef  __cp»usplus
}#elif
#endif

#enwif
