/**
 * @file      mathexpr.c
 * @brief     Parsing and evaluation of mathematical expressions.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

/******************************************************************************
**  MODULE:        MATHEXPR.C
**  PROJECT:       EPA SWMM5
**  DESCRIPTION:   Evaluates symbolic mathematical expression consisting
**                 of numbers, variable names, math functions & arithmetic
**                 operators.
**  AUTHORS:       L. Rossman, US EPA - NRMRL
**                 F. Shang, University of Cincinnati
**  VERSION:       5.2.2
**  LAST UPDATE:   09/02/2022
**  BUG FIXES:     Problems related to '^' operator (F. Shang, 09/02/2022)
******************************************************************************/
/*
**   Operand codes:
** 	   1 = (
** 	   2 = )
** 	   3 = +
** 	   4 = - (subtraction)
** 	   5 = *
** 	   6 = /
** 	   7 = number
** 	   8 = user-defined variable
** 	   9 = - (negative)
**	  10 = cos
**	  11 = sin
**	  12 = tan
**	  13 = cot
**	  14 = abs
**	  15 = sgn
**	  16 = sqrt
**	  17 = log
**	  18 = exp
**	  19 = asin
**	  20 = acos
**	  21 = atan
**	  22 = acot
**        23 = sinh
**	  24 = cosh
**	  25 = tanh
**	  26 = coth
**	  27 = log10
**        28 = step (x<=0 ? 0 : 1)
**	  31 = ^
******************************************************************************/
#define _CRT_SECURE_NO_DEPRECATE

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "mathexpr.h"

#define MAX_STACK_SIZE 1024

//  Local declarations
//--------------------
//  Structure for binary tree representation of math expression
struct TreeNode {
    int opcode;             // operator code
    int ivar;               // variable index
    double fvalue;          // numerical value
    struct TreeNode* left;  // left sub-tree of tokenized formula
    struct TreeNode* right; // right sub-tree of tokenized formula
};
typedef struct TreeNode ExprTree;

// Local variables
//----------------
static int Err;
static int Bc;
static int PrevLex, CurLex;
static int Len, Pos;
static char* S;
static char Token[255];
static int Ivar;
static double Fvalue;

// math function names
char* MathFunc[] = {"COS",  "SIN",  "TAN",  "COT",  "ABS",  "SGN",  "SQRT", "LOG",   "EXP",  "ASIN",
                    "ACOS", "ATAN", "ACOT", "SINH", "COSH", "TANH", "COTH", "LOG10", "STEP", NULL};

// Local functions
//----------------
static int sametext(char*, char*);
static int isDigit(char);
static int isLetter(char);
static void getToken(void);
static int getMathFunc(void);
static int getVariable(void);
static int getOperand(void);
static int getLex(void);
static double getNumber(void);
static ExprTree* newNode(void);
static ExprTree* getSingleOp(int*);
static ExprTree* getOp(int*);
static ExprTree* getTree(void);
static void traverseTree(ExprTree*, MathExpr**);
static void deleteTree(ExprTree*);

// Callback functions
static int (*getVariableIndex)(char*); // return index of named variable

//=============================================================================

/**
 * @brief Compare nonempty parser identifiers without case sensitivity.
 * @param[in] s1 Non-null borrowed null-terminated first identifier.
 * @param[in] s2 Non-null borrowed null-terminated second identifier.
 * @return One for equal case-folded text, zero otherwise.
 * @pre Identifier characters are valid unsigned-char values for toupper(),
 * and both strings are nonempty. No text is changed.
 */
int sametext(char* s1, char* s2)
/*
**  Purpose:
**    performs case insensitive comparison of two strings.
**
**  Input:
**    s1 = character string
**    s2 = character string.
**
**  Returns:
**    1 if strings are the same, 0 otherwise.
*/
{
    int i;
    for (i = 0; toupper(s1[i]) == toupper(s2[i]); i++)
        if (!s1[i + 1] && !s2[i + 1])
            return (1);
    return (0);
}

//=============================================================================

/**
 * @brief Classify a character for expression tokenization.
 * @param[in] c Character value to inspect.
 * @return One when c is an ASCII decimal digit 0..9, zero otherwise.
 */
int isDigit(char c) {
    if (c >= '1' && c <= '9')
        return 1;
    if (c == '0')
        return 1;
    return 0;
}

//=============================================================================

/**
 * @brief Classify a character for expression tokenization.
 * @param[in] c Character value to inspect.
 * @return One when c is an ASCII letter or underscore, zero otherwise.
 */
int isLetter(char c) {
    if (c >= 'a' && c <= 'z')
        return 1;
    if (c >= 'A' && c <= 'Z')
        return 1;
    if (c == '_')
        return 1;
    return 0;
}

//=============================================================================

/**
 * @brief Copy an identifier from shared input S into the shared Token buffer.
 * @pre Pos points to an identifier start in null-terminated S, Len is its
 * length, and the identifier occupies fewer than 255 bytes including the terminator.
 * @post Token contains letters/digits/underscores and Pos points to the
 * last token character so getLex() can advance past it. No bounds status is returned.
 */
void getToken() {
    char c[] = " ";
    Token[0] = '\0';
    while (Pos <= Len && (isLetter(S[Pos]) || isDigit(S[Pos]))) {
        c[0] = S[Pos];
        strcat(Token, c);
        Pos++;
    }
    Pos--;
}

//=============================================================================

/**
 * @brief Resolve the shared Token as a case-insensitive built-in function name.
 * @return Function opcode 10..28 in MathFunc order, or zero for an unrecognized name.
 * @pre Token is a nonempty null-terminated identifier. No parser state is changed.
 */
int getMathFunc() {
    int i = 0;
    while (MathFunc[i] != NULL) {
        if (sametext(MathFunc[i], Token))
            return i + 10;
        i++;
    }
    return (0);
}

//=============================================================================

/**
 * @brief Bind the shared Token through the parser's optional variable-index callback.
 * @return Variable opcode 8 for a nonnegative callback index; zero if no
 * callback exists or it returns a negative index.
 * @post Stores a callback result in shared Ivar when the callback is present.
 * Token storage is borrowed for the synchronous callback and must not be retained.
 */
int getVariable() {
    if (!getVariableIndex)
        return 0;
    Ivar = getVariableIndex(Token);
    if (Ivar >= 0)
        return 8;
    return 0;
}

//=============================================================================

/**
 * @brief Scan a numeric literal from shared parser input, including an exponent.
 * @return atof() value, or zero when the exponent is incomplete; no separate
 * numeric-error status accompanies that zero.
 * @pre S/Len/Pos identify the current token, and the literal fits in the
 * 255-byte local buffer including terminator. Numeric range is not validated.
 * @post Leaves Pos at the last scanned character; may set shared Err for
 * lexical issues. Input text is not modified.
 */
double getNumber() {
    char c[] = " ";
    char sNumber[255];
    int errflag = 0;
    int decimalCount = 0;

    /* --- get whole number portion of number */
    sNumber[0] = '\0';
    while (Pos < Len && isDigit(S[Pos])) {
        c[0] = S[Pos];
        strcat(sNumber, c);
        Pos++;
    }

    /* --- get fractional portion of number */
    if (Pos < Len) {
        if (S[Pos] == '.') {
            decimalCount++;
            if (decimalCount > 1)
                Err = 1;
            strcat(sNumber, ".");
            Pos++;
            while (Pos < Len && isDigit(S[Pos])) {
                c[0] = S[Pos];
                strcat(sNumber, c);
                Pos++;
            }
        }

        /* --- get exponent */
        if (Pos < Len && (S[Pos] == 'e' || S[Pos] == 'E')) {
            strcat(sNumber, "E");
            Pos++;
            if (Pos >= Len)
                errflag = 1;
            else {
                if (S[Pos] == '-' || S[Pos] == '+') {
                    c[0] = S[Pos];
                    strcat(sNumber, c);
                    Pos++;
                }
                if (Pos >= Len || !isDigit(S[Pos]))
                    errflag = 1;
                else
                    while (Pos < Len && isDigit(S[Pos])) {
                        c[0] = S[Pos];
                        strcat(sNumber, c);
                        Pos++;
                    }
            }
        }
    }
    Pos--;
    if (errflag)
        return 0;
    else
        return atof(sNumber);
}

//=============================================================================

/**
 * @brief Classify the operator at shared input position and handle a signed numeric literal.
 * @return Parenthesis/operator opcode, number opcode 7 for a folded negative
 * number, or zero when the character is not a recognized operator.
 * @pre Pos identifies a readable character in S and parser lexical state is initialized.
 * @post A folded number updates shared Fvalue/Pos; otherwise no token is consumed.
 */
int getOperand() {
    int code;
    switch (S[Pos]) {
    case '(':
        code = 1;
        break;
    case ')':
        code = 2;
        break;
    case '+':
        code = 3;
        break;
    case '-':
        code = 4;
        if (Pos < Len - 1 && isDigit(S[Pos + 1]) && (CurLex <= 6 || CurLex == 31)) {
            Pos++;
            Fvalue = -getNumber();
            code = 7;
        }
        break;
    case '*':
        code = 5;
        break;
    case '/':
        code = 6;
        break;
    case '^':
        code = 31;
        break;
    default:
        code = 0;
    }
    return code;
}

//=============================================================================

/**
 * @brief Consume the next operator, function, variable or number token.
 * @return Recognized opcode, or zero for end-of-input/unrecognized text.
 * @post Advances shared Pos and updates PrevLex/CurLex plus Token, Ivar or
 * Fvalue as needed. Skips literal space characters, not arbitrary whitespace.
 * @pre Shared parser state was initialized by mathexpr_create().
 */
int getLex() {
    int n;

    /* --- skip spaces */
    while (Pos < Len && S[Pos] == ' ')
        Pos++;
    if (Pos >= Len)
        return 0;

    /* --- check for operand */
    n = getOperand();

    /* --- check for function/variable/number */
    if (n == 0) {
        if (isLetter(S[Pos])) {
            getToken();
            n = getMathFunc();
            if (n == 0)
                n = getVariable();
        } else if (S[Pos] == '.' || isDigit(S[Pos])) {
            n = 7;
            Fvalue = getNumber();
        }
    }
    Pos++;
    PrevLex = CurLex;
    CurLex = n;
    return n;
}

//=============================================================================

/**
 * @brief Allocate an empty expression-tree node.
 * @return Owned initialized ExprTree node, or NULL on malloc failure.
 * @post Allocation failure sets shared Err to 2. Node children are NULL,
 * operator/value zero, and variable index -1; release via deleteTree().
 */
ExprTree* newNode() {
    ExprTree* node;
    node = (ExprTree*)malloc(sizeof(ExprTree));
    if (!node)
        Err = 2;
    else {
        node->opcode = 0;
        node->ivar = -1;
        node->fvalue = 0.;
        node->left = NULL;
        node->right = NULL;
    }
    return node;
}

//=============================================================================

/**
 * @brief Parse a single value, function, parenthesized expression or exponent chain.
 * @param[in,out] lex Non-null borrowed current opcode slot; advanced to the
 * first token following the parsed operand/exponent expression.
 * @return Owned subtree, or NULL for detected syntax/allocation errors.
 * @post Advances shared lexical/parenthesis state and may set Err.
 * @note Allocation failure handling follows the legacy parser and is not
 * complete; callers use mathexpr_create() rather than invoking this helper directly.
 */
ExprTree* getSingleOp(int* lex) {
    int opcode;
    ExprTree* left;
    ExprTree* node;

    /* --- open parenthesis, so continue to grow the tree */
    if (*lex == 1) {
        Bc++;
        left = getTree();
    }

    else {
        /* --- Error if not a singleton operand */
        if (*lex < 7 || *lex == 9 || *lex > 30) {
            Err = 1;
            return NULL;
        }

        opcode = *lex;

        /* --- simple number or variable name */
        if (*lex == 7 || *lex == 8) {
            left = newNode();
            left->opcode = opcode;
            if (*lex == 7)
                left->fvalue = Fvalue;
            if (*lex == 8)
                left->ivar = Ivar;
        }

        /* --- function which must have a '(' after it */
        else {
            *lex = getLex();
            if (*lex != 1) {
                Err = 1;
                return NULL;
            }
            Bc++;
            left = newNode();
            left->left = getTree();
            left->opcode = opcode;
        }
    }
    *lex = getLex();

    /* --- exponentiation */
    if (*lex == 31) {
        node = newNode();
        node->left = left;
        node->opcode = *lex;
        *lex = getLex();
        node->right = getSingleOp(lex);
        left = node;
    }
    return left;
}

//=============================================================================

/**
 * @brief Parse multiplicative terms and optional leading unary signs.
 * @param[in,out] lex Non-null borrowed opcode slot receiving the token
 * following the parsed term; the first token is read internally.
 * @return Owned term subtree or NULL on a detected parser error.
 * @post Updates shared Pos/PrevLex/CurLex/Err and builds multiplication,
 * division or unary-negation nodes; release returned nodes through deleteTree().
 */
ExprTree* getOp(int* lex) {
    int opcode;
    ExprTree* left;
    ExprTree* right;
    ExprTree* node;
    int neg = 0;

    *lex = getLex();
    if (PrevLex == 0 || PrevLex == 1) {
        if (*lex == 4) {
            neg = 1;
            *lex = getLex();
        } else if (*lex == 3)
            *lex = getLex();
    }
    left = getSingleOp(lex);
    while (*lex == 5 || *lex == 6) {
        opcode = *lex;
        *lex = getLex();
        right = getSingleOp(lex);
        node = newNode();
        if (Err)
            return NULL;
        node->left = left;
        node->right = right;
        node->opcode = opcode;
        left = node;
    }
    if (neg) {
        node = newNode();
        if (Err)
            return NULL;
        node->left = left;
        node->right = NULL;
        node->opcode = 9;
        left = node;
    }
    return left;
}

//=============================================================================

/**
 * @brief Parse an additive expression from the shared lexical cursor.
 * @return Owned expression-tree root, possibly partial when Err is set.
 * @post Consumes input through end or a closing parenthesis, updates Bc,
 * and sets shared Err for invalid operator sequences.
 * @pre mathexpr_create() initialized input, cursor and variable binding.
 * Returned trees remain the caller's responsibility even after a parse error.
 */
ExprTree* getTree() {
    int lex;
    int opcode;
    ExprTree* left;
    ExprTree* right;
    ExprTree* node;

    left = getOp(&lex);
    for (;;) {
        if (lex == 0 || lex == 2) {
            if (lex == 2)
                Bc--;
            break;
        }

        if (lex != 3 && lex != 4) {
            Err = 1;
            break;
        }

        opcode = lex;
        right = getOp(&lex);
        node = newNode();
        if (Err)
            break;
        node->left = left;
        node->right = right;
        node->opcode = opcode;
        left = node;
    }
    return left;
}

//=============================================================================

/**
 * @brief Append a binary expression tree to a postfix doubly linked token list.
 * @param[in] tree Borrowed root; NULL appends nothing. Traversal is recursive.
 * @param[in,out] expr Non-null pointer to the current list tail, initially
 * NULL for an empty list; replaced with each newly allocated tail node.
 * @post Allocated MathExpr nodes belong to the expression owner and must
 * be released with mathexpr_delete(); the input tree is unchanged.
 * @note Allocation failure is not returned as a status and may leave a
 * partial/truncated list. Callers must follow the parser's normal lifecycle.
 */
void traverseTree(ExprTree* tree, MathExpr** expr)
// Converts binary tree to linked list (postfix format)
{
    MathExpr* node;
    if (tree == NULL)
        return;
    traverseTree(tree->left, expr);
    traverseTree(tree->right, expr);
    node = (MathExpr*)malloc(sizeof(MathExpr));
    if (node) {
        node->fvalue = tree->fvalue;
        node->opcode = tree->opcode;
        node->ivar = tree->ivar;
        node->next = NULL;
        node->prev = (*expr);
    }
    if (*expr)
        (*expr)->next = node;
    (*expr) = node;
}

//=============================================================================

/**
 * @brief Recursively release an exclusively owned expression tree.
 * @param[in] tree Owned root to release, or NULL for no action.
 * @post All descendant/tree pointers become invalid; unrelated postfix
 * MathExpr nodes are not owned by this tree and are not freed here.
 */
void deleteTree(ExprTree* tree) {
    if (tree) {
        if (tree->left)
            deleteTree(tree->left);
        if (tree->right)
            deleteTree(tree->right);
        free(tree);
    }
}

//=============================================================================

// Turn on "precise" floating point option
#pragma float_control(precise, on, push)

/**
 * @brief Evaluate a legacy postfix expression using a local operand stack.
 * @param[in] expr Borrowed valid linked expression; NULL evaluates to zero.
 * @param[in] getVariableValue Optional synchronous index-to-value callback;
 * NULL supplies zero for every variable. The callback is not retained.
 * @return Final expression value; malformed stack underflow and NaN results
 * are mapped to zero. Infinite results are not changed.
 * @pre Expression operand counts and peak stack occupancy fit the fixed
 * MAX_STACK_SIZE array; stack overflow is not safely recovered by this routine.
 * @note The local stack permits recursive expression evaluation. Domain
 * behavior varies by opcode: negative sqrt and nonpositive log become zero,
 * while other illegal math may become NaN and then zero at the final check.
 */
double mathexpr_eval(MathExpr* expr, double (*getVariableValue)(int))
//  Mathematica expression evaluation using a stack
{

    // --- Note: the ExprStack array must be declared locally and not globally
    //     since this function can be called recursively.

    double ExprStack[MAX_STACK_SIZE];
    MathExpr* node = expr;
    double r1, r2;
    int stackindex = 0;

    ExprStack[0] = 0.0;
    while (node != NULL && stackindex >= 0) {
        switch (node->opcode) {
        case 3:
            r1 = ExprStack[stackindex];
            stackindex--;
            if (stackindex < 0)
                break;
            r2 = ExprStack[stackindex];
            ExprStack[stackindex] = r2 + r1;
            break;

        case 4:
            r1 = ExprStack[stackindex];
            stackindex--;
            if (stackindex < 0)
                break;
            r2 = ExprStack[stackindex];
            ExprStack[stackindex] = r2 - r1;
            break;

        case 5:
            r1 = ExprStack[stackindex];
            stackindex--;
            if (stackindex < 0)
                break;
            r2 = ExprStack[stackindex];
            ExprStack[stackindex] = r2 * r1;
            break;

        case 6:
            r1 = ExprStack[stackindex];
            stackindex--;
            if (stackindex < 0)
                break;
            r2 = ExprStack[stackindex];
            ExprStack[stackindex] = r2 / r1;
            break;

        case 7:
            stackindex++;
            if (stackindex >= MAX_STACK_SIZE)
                break;
            ExprStack[stackindex] = node->fvalue;
            break;

        case 8:
            if (getVariableValue != NULL) {
                r1 = getVariableValue(node->ivar);
            } else
                r1 = 0.0;
            stackindex++;
            if (stackindex >= MAX_STACK_SIZE)
                break;
            ExprStack[stackindex] = r1;
            break;

        case 9:
            ExprStack[stackindex] = -ExprStack[stackindex];
            break;

        case 10:
            r1 = ExprStack[stackindex];
            r2 = cos(r1);
            ExprStack[stackindex] = r2;
            break;

        case 11:
            r1 = ExprStack[stackindex];
            r2 = sin(r1);
            ExprStack[stackindex] = r2;
            break;

        case 12:
            r1 = ExprStack[stackindex];
            r2 = tan(r1);
            ExprStack[stackindex] = r2;
            break;

        case 13:
            r1 = ExprStack[stackindex];
            if (r1 == 0.0)
                r2 = 0.0;
            else
                r2 = 1.0 / tan(r1);
            ExprStack[stackindex] = r2;
            break;

        case 14:
            r1 = ExprStack[stackindex];
            r2 = fabs(r1);
            ExprStack[stackindex] = r2;
            break;

        case 15:
            r1 = ExprStack[stackindex];
            if (r1 < 0.0)
                r2 = -1.0;
            else if (r1 > 0.0)
                r2 = 1.0;
            else
                r2 = 0.0;
            ExprStack[stackindex] = r2;
            break;

        case 16:
            r1 = ExprStack[stackindex];
            if (r1 < 0.0)
                r2 = 0.0;
            else
                r2 = sqrt(r1);
            ExprStack[stackindex] = r2;
            break;

        case 17:
            r1 = ExprStack[stackindex];
            if (r1 <= 0)
                r2 = 0.0;
            else
                r2 = log(r1);
            ExprStack[stackindex] = r2;
            break;

        case 18:
            r1 = ExprStack[stackindex];
            r2 = exp(r1);
            ExprStack[stackindex] = r2;
            break;

        case 19:
            r1 = ExprStack[stackindex];
            r2 = asin(r1);
            ExprStack[stackindex] = r2;
            break;

        case 20:
            r1 = ExprStack[stackindex];
            r2 = acos(r1);
            ExprStack[stackindex] = r2;
            break;

        case 21:
            r1 = ExprStack[stackindex];
            r2 = atan(r1);
            ExprStack[stackindex] = r2;
            break;

        case 22:
            r1 = ExprStack[stackindex];
            r2 = 1.57079632679489661923 - atan(r1);
            ExprStack[stackindex] = r2;
            break;

        case 23:
            r1 = ExprStack[stackindex];
            r2 = (exp(r1) - exp(-r1)) / 2.0;
            ExprStack[stackindex] = r2;
            break;

        case 24:
            r1 = ExprStack[stackindex];
            r2 = (exp(r1) + exp(-r1)) / 2.0;
            ExprStack[stackindex] = r2;
            break;

        case 25:
            r1 = ExprStack[stackindex];
            r2 = (exp(r1) - exp(-r1)) / (exp(r1) + exp(-r1));
            ExprStack[stackindex] = r2;
            break;

        case 26:
            r1 = ExprStack[stackindex];
            r2 = (exp(r1) + exp(-r1)) / (exp(r1) - exp(-r1));
            ExprStack[stackindex] = r2;
            break;

        case 27:
            r1 = ExprStack[stackindex];
            if (r1 == 0.0)
                r2 = 0.0;
            else
                r2 = log10(r1);
            ExprStack[stackindex] = r2;
            break;

        case 28:
            r1 = ExprStack[stackindex];
            if (r1 <= 0.0)
                r2 = 0.0;
            else
                r2 = 1.0;
            ExprStack[stackindex] = r2;
            break;

        case 31:
            r1 = ExprStack[stackindex];
            stackindex--;
            if (stackindex < 0)
                break;
            r2 = ExprStack[stackindex];
            if (r2 <= 0.0)
                r2 = 0.0;
            else
                r2 = pow(r2, r1);
            ExprStack[stackindex] = r2;
            break;
        }
        node = node->next;
    }
    if (stackindex >= 0)
        r1 = ExprStack[stackindex];
    else
        r1 = 0.0;

    // Set result to 0 if it is NaN due to an illegal math op
    if (r1 != r1)
        r1 = 0.0;

    return r1;
}

// Turn off "precise" floating point option
#pragma float_control(pop)

//=============================================================================

/**
 * @brief Recursively release a complete postfix MathExpr list.
 * @param[in] expr Owned list head, or NULL for no action.
 * @post All reachable next-linked nodes are freed. The caller must discard
 * its original pointer; deep expressions consume recursive call-stack space.
 */
void mathexpr_delete(MathExpr* expr) {
    if (expr)
        mathexpr_delete(expr->next);
    free(expr);
}

//=============================================================================

/**
 * @brief Compile an infix formula into an owned postfix expression list.
 * @param[in] formula Non-null null-terminated input; identifiers and numeric
 * literals must fit the parser's 255-byte token buffers and strlen must fit in int.
 * @param[in] getVar Optional synchronous variable-name-to-index callback;
 * negative indices reject names. The temporary token pointer must not be retained.
 * @return Owned expression head to release with mathexpr_delete(), or NULL
 * for detected syntax/parenthesis errors or an empty result.
 * @note The compiled list stores values/indices and evaluates independently
 * of the input/callback. Parsing uses shared globals and is not reentrant or
 * thread-safe; allocation failures are not comprehensively recovered.
 */
MathExpr* mathexpr_create(char* formula, int (*getVar)(char*)) {
    ExprTree* tree;
    MathExpr* expr = NULL;
    MathExpr* result = NULL;
    getVariableIndex = getVar;
    Err = 0;
    PrevLex = 0;
    CurLex = 0;
    S = formula;
    Len = (int)strlen(S);
    Pos = 0;
    Bc = 0;
    tree = getTree();
    if (Bc == 0 && Err == 0) {
        traverseTree(tree, &expr);
        while (expr) {
            result = expr;
            expr = expr->prev;
        }
    }
    deleteTree(tree);
    return result;
}
