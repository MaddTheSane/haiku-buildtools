/* { dg-do compile } */
/* { dg-options "-O3 -ftree-vectorize" } */
unsigned short a, e;
int *b, *d;
int c;
extern int fn2();
void fn1 () {
  void *f;
  for (;;) {
    fn2 ();
    b = f;
    e = 0;
    for (; e < a; ++e)
      b[e] = d[e * c];
  }
}
