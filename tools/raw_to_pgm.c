/* SPDX-License-Identifier: 0BSD */

#include <stdio.h>
#include <stdlib.h>

int
main (int argc, char **argv)
{
  unsigned char frame[70 * 57];
  FILE *input;
  FILE *output;

  if (argc != 3)
    return EXIT_FAILURE;

  input = fopen (argv[1], "rb");
  output = fopen (argv[2], "wb");
  if (!input || !output || fread (frame, 1, sizeof frame, input) != sizeof frame)
    return EXIT_FAILURE;

  fprintf (output, "P5\n70 57\n255\n");
  fwrite (frame, 1, sizeof frame, output);
  fclose (input);
  fclose (output);
  return EXIT_SUCCESS;
}
