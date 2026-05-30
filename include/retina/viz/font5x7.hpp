#pragma once
#include <cstdint>

namespace retina {

// A minimal 5x7 bitmap font for the M4 HUD — digits, uppercase letters, and the
// handful of symbols the overlay uses. Each glyph is 7 rows; the low 5 bits of
// each row are the columns (bit 4 = leftmost). Unsupported chars render blank.
//
// Uppercase-only keeps the table small; the HUD is written in uppercase.
inline const uint8_t* glyph5x7(char c) {
    static const uint8_t BLANK[7] = {0, 0, 0, 0, 0, 0, 0};

    static const uint8_t DOT[7]  = {0,0,0,0,0,0b00110,0b00110};
    static const uint8_t COLON[7]= {0,0b00110,0b00110,0,0b00110,0b00110,0};
    static const uint8_t PCT[7]  = {0b11000,0b11001,0b00010,0b00100,0b01000,0b10011,0b00011};
    static const uint8_t DASH[7] = {0,0,0,0b11111,0,0,0};
    static const uint8_t SLASH[7]= {0b00001,0b00010,0b00100,0b00100,0b01000,0b10000,0b10000};

    static const uint8_t N0[7] = {0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110};
    static const uint8_t N1[7] = {0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110};
    static const uint8_t N2[7] = {0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111};
    static const uint8_t N3[7] = {0b11111,0b00010,0b00100,0b00010,0b00001,0b10001,0b01110};
    static const uint8_t N4[7] = {0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010};
    static const uint8_t N5[7] = {0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110};
    static const uint8_t N6[7] = {0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110};
    static const uint8_t N7[7] = {0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000};
    static const uint8_t N8[7] = {0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110};
    static const uint8_t N9[7] = {0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100};

    static const uint8_t A[7] = {0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001};
    static const uint8_t B[7] = {0b11110,0b10001,0b10001,0b11110,0b10001,0b10001,0b11110};
    static const uint8_t C[7] = {0b01110,0b10001,0b10000,0b10000,0b10000,0b10001,0b01110};
    static const uint8_t D[7] = {0b11100,0b10010,0b10001,0b10001,0b10001,0b10010,0b11100};
    static const uint8_t E[7] = {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111};
    static const uint8_t F[7] = {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b10000};
    static const uint8_t G[7] = {0b01110,0b10001,0b10000,0b10111,0b10001,0b10001,0b01111};
    static const uint8_t H[7] = {0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001};
    static const uint8_t I[7] = {0b01110,0b00100,0b00100,0b00100,0b00100,0b00100,0b01110};
    static const uint8_t J[7] = {0b00111,0b00010,0b00010,0b00010,0b00010,0b10010,0b01100};
    static const uint8_t K[7] = {0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001};
    static const uint8_t L[7] = {0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111};
    static const uint8_t M[7] = {0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001};
    static const uint8_t N[7] = {0b10001,0b10001,0b11001,0b10101,0b10011,0b10001,0b10001};
    static const uint8_t O[7] = {0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110};
    static const uint8_t P[7] = {0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000};
    static const uint8_t Q[7] = {0b01110,0b10001,0b10001,0b10001,0b10101,0b10010,0b01101};
    static const uint8_t R[7] = {0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001};
    static const uint8_t S[7] = {0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110};
    static const uint8_t T[7] = {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100};
    static const uint8_t U[7] = {0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110};
    static const uint8_t V[7] = {0b10001,0b10001,0b10001,0b10001,0b10001,0b01010,0b00100};
    static const uint8_t W[7] = {0b10001,0b10001,0b10001,0b10101,0b10101,0b11011,0b10001};
    static const uint8_t X[7] = {0b10001,0b10001,0b01010,0b00100,0b01010,0b10001,0b10001};
    static const uint8_t Y[7] = {0b10001,0b10001,0b01010,0b00100,0b00100,0b00100,0b00100};
    static const uint8_t Z[7] = {0b11111,0b00001,0b00010,0b00100,0b01000,0b10000,0b11111};

    switch (c) {
        case ' ': return BLANK;
        case '.': return DOT;
        case ':': return COLON;
        case '%': return PCT;
        case '-': return DASH;
        case '/': return SLASH;
        case '0': return N0; case '1': return N1; case '2': return N2;
        case '3': return N3; case '4': return N4; case '5': return N5;
        case '6': return N6; case '7': return N7; case '8': return N8;
        case '9': return N9;
        case 'A': return A; case 'B': return B; case 'C': return C;
        case 'D': return D; case 'E': return E; case 'F': return F;
        case 'G': return G; case 'H': return H; case 'I': return I;
        case 'J': return J; case 'K': return K; case 'L': return L;
        case 'M': return M; case 'N': return N; case 'O': return O;
        case 'P': return P; case 'Q': return Q; case 'R': return R;
        case 'S': return S; case 'T': return T; case 'U': return U;
        case 'V': return V; case 'W': return W; case 'X': return X;
        case 'Y': return Y; case 'Z': return Z;
        default:  return BLANK;
    }
}

} // namespace retina
