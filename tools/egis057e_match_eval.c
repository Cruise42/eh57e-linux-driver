/* SPDX-License-Identifier: 0BSD */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define W 70
#define H 57
#define N (W * H)

static int
read_frame (const char *path, unsigned char *image, long offset)
{
  FILE *f = fopen (path, "rb");
  int ok;
  if (!f || fseek (f, offset, SEEK_SET))
    return 0;
  ok = fread (image, 1, N, f) == N;
  fclose (f);
  return ok;
}

/* Zero-mean normalized cross correlation, maximizing over translations. */
static double
score (const unsigned char *a, const unsigned char *b, int *best_dx, int *best_dy)
{
  double best = -1.0;
  for (int dy = -12; dy <= 12; dy++)
    for (int dx = -12; dx <= 12; dx++)
      {
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
        int count = 0;
        for (int y = 4; y < H - 4; y++)
          for (int x = 4; x < W - 4; x++)
            {
              int bx = x + dx, by = y + dy;
              double va, vb;
              if (bx < 4 || bx >= W - 4 || by < 4 || by >= H - 4)
                continue;
              /* Horizontal and vertical gradients suppress illumination. */
              va = (a[y * W + x + 1] - a[y * W + x - 1]) +
                   (a[(y + 1) * W + x] - a[(y - 1) * W + x]);
              vb = (b[by * W + bx + 1] - b[by * W + bx - 1]) +
                   (b[(by + 1) * W + bx] - b[(by - 1) * W + bx]);
              sa += va; sb += vb; saa += va * va; sbb += vb * vb;
              sab += va * vb; count++;
            }
        {
          double numerator = sab - sa * sb / count;
          double denominator = sqrt ((saa - sa * sa / count) *
                                     (sbb - sb * sb / count));
          double value = denominator > 0 ? numerator / denominator : -1;
          if (value > best)
            { best = value; *best_dx = dx; *best_dy = dy; }
        }
      }
  return best;
}

int
main (int argc, char **argv)
{
  unsigned char a[N], b[N];
  int dx = 0, dy = 0;
  long oa = argc > 3 ? strtol (argv[3], NULL, 0) : 0;
  long ob = argc > 4 ? strtol (argv[4], NULL, 0) : 0;
  if (argc < 3 || !read_frame (argv[1], a, oa) || !read_frame (argv[2], b, ob))
    return EXIT_FAILURE;
  printf ("%.6f dx=%d dy=%d\n", score (a, b, &dx, &dy), dx, dy);
  return EXIT_SUCCESS;
}
