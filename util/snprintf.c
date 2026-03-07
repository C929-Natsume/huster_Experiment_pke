/*
 * vsnprintf() is borrowed from pk.
 */

// #include <stdint.h>
// #include <stdarg.h>
// #include <stdbool.h>

#include "util/snprintf.h"

int32 vsnprintf(char *out, size_t n, const char *s, va_list vl)
{
  bool format = FALSE;
  bool longarg = FALSE;
  size_t pos = 0;

  for (; *s; s++)
  {
    if (format)
    {
      switch (*s)
      {
      case 'l':
        longarg = TRUE;
        break;
      case 'p':
        longarg = TRUE;
        if (++pos < n)
          out[pos - 1] = '0';
        if (++pos < n)
          out[pos - 1] = 'x';
      case 'x':
      {
        long num = longarg ? va_arg(vl, long) : va_arg(vl, int);
        for (int i = 2 * (longarg ? sizeof(long) : sizeof(int)) - 1; i >= 0; i--)
        {
          int d = (num >> (4 * i)) & 0xF;
          if (++pos < n)
            out[pos - 1] = (d < 10 ? '0' + d : 'a' + d - 10);
        }
        longarg = FALSE;
        format = FALSE;
        break;
      }
      case 'd':
      {
        long num = longarg ? va_arg(vl, long) : va_arg(vl, int);
        if (num < 0)
        {
          num = -num;
          if (++pos < n)
            out[pos - 1] = '-';
        }
        long digits = 1;
        for (long nn = num; nn /= 10; digits++)
          ;
        for (int i = digits - 1; i >= 0; i--)
        {
          if (pos + i + 1 < n)
            out[pos + i] = '0' + (num % 10);
          num /= 10;
        }
        pos += digits;
        longarg = FALSE;
        format = FALSE;
        break;
      }
      case 's':
      {
        const char *s2 = va_arg(vl, const char *);
        while (*s2)
        {
          if (++pos < n)
            out[pos - 1] = *s2;
          s2++;
        }
        longarg = FALSE;
        format = FALSE;
        break;
      }
      case 'c':
      {
        if (++pos < n)
          out[pos - 1] = (char)va_arg(vl, int);
        longarg = FALSE;
        format = FALSE;
        break;
      }
      default:
        break;
      }
    }
    else if (*s == '%')
      format = TRUE;
    else if (++pos < n)
      out[pos - 1] = *s;
  }
  if (pos < n)
    out[pos] = 0;
  else if (n)
    out[n - 1] = 0;
  return pos;
}

// int32 vsnscanf(const char *in, const char *format, va_list vl)
// {
//   bool longarg = FALSE;
//   int count = 0;

//   while (*format && *in)
//   {
//     if (*format == '%')
//     {
//       format++;

//       if (*format == 'l')
//       {
//         longarg = TRUE;
//         format++;
//       }

//       switch (*format)
//       {
//       case 'd':
//       {
//         long val = 0;
//         int neg = 0;

//         if (*in == '-')
//         {
//           neg = 1;
//           in++;
//         }

//         while (*in >= '0' && *in <= '9')
//         {
//           val = val * 10 + (*in - '0');
//           in++;
//         }

//         if (neg)
//           val = -val;

//         if (longarg)
//         {
//           long *p = va_arg(vl, long *);
//           *p = val;
//         }
//         else
//         {
//           int *p = va_arg(vl, int *);
//           *p = (int)val;
//         }

//         count++;
//         longarg = FALSE;
//         break;
//       }

//       case 'x':
//       case 'p':
//       {
//         long val = 0;

//         if (*in == '0' && (*(in + 1) == 'x' || *(in + 1) == 'X'))
//           in += 2;

//         while (1)
//         {
//           char c = *in;
//           int digit;

//           if (c >= '0' && c <= '9')
//             digit = c - '0';
//           else if (c >= 'a' && c <= 'f')
//             digit = c - 'a' + 10;
//           else if (c >= 'A' && c <= 'F')
//             digit = c - 'A' + 10;
//           else
//             break;

//           val = (val << 4) + digit;
//           in++;
//         }

//         if (longarg || *format == 'p')
//         {
//           long *p = va_arg(vl, long *);
//           *p = val;
//         }
//         else
//         {
//           int *p = va_arg(vl, int *);
//           *p = (int)val;
//         }

//         count++;
//         longarg = FALSE;
//         break;
//       }

//       case 's':
//       {
//         char *buf = va_arg(vl, char *);

//         while (*in && *in != ' ' && *in != '\n' && *in != '\t')
//         {
//           *buf++ = *in++;
//         }
//         *buf = '\0';

//         count++;
//         break;
//       }

//       case 'c':
//       {
//         char *p = va_arg(vl, char *);
//         *p = *in++;
//         count++;
//         break;
//       }

//       default:
//         break;
//       }

//       format++;
//     }
//     else
//     {
//       if (*format == *in)
//       {
//         format++;
//         in++;
//       }
//       else
//       {
//         break;
//       }
//     }
//   }

//   return count;
// }