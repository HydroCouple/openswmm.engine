/*!
 * \file controls.c
 * \brief Rule-based controls functions.
 * \author L. Rossman
 * \date Created: 2022-06-01
 * \date Last updated: 2024-12-30
 * \version 5.3
 * \details
 * Control rules have the format:
 *   RULE name
 *   IF <premise>
 *   AND / OR <premise>
 *   etc.
 *   THEN <action>
 *   AND  <action>
 *   etc.
 *   ELSE <action>
 *   AND  <action>
 *   etc.
 *   PRIORITY <p>
 *
 * <premise> consists of:
 * <variable> <relational operator> value / <variable>
 * where <variable> is <object type> <id name> <attribute>
 * E.g.: Node 123 Depth > 4.5
 *       Node 456 Depth < Node 123 Depth
 *
 * <action> consists of:
 * <variable> = setting
 * E.g.: Pump abc status = OFF
 *       Weir xyz setting = 0.5
 *
 * Update History
 * ==============
 * Build 5.1.008:
 * - Support added for r.h.s. variables in rule premises.
 * - Node volume added as a premise variable.
 * Build 5.1.009:
 * - Fixed problem with parsing a RHS premise variable.
 * Build 5.1.010:
 * - Support added for link TIMEOPEN & TIMECLOSED premises.
 * Build 5.1.011:
 * - Support added for DAYOFYEAR attribute.
 * - Modulated controls no longer included in reported control actions.
 * Build 5.2.0:
 * - Additional attributes added to condition clauses.
 * - Support added for named variables in condition clauses.
 * - Support added for math expressions in condition clauses.
 * Build 5.2.1:
 * - A refactoring bug from 5.2.0 causing duplicate actions to be added
 *   to the list of control actions to take was fixed.
 */

/*!
 * \def _CRT_SECURE_NO_DEPRECATE
 * \brief Define to prevent deprecation warnings from MS Visual C++ compilers
 */
#define _CRT_SECURE_NO_DEPRECATE

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "headers.h"

//-----------------------------------------------------------------------------
//  Constants
//-----------------------------------------------------------------------------
/*!
 * \defgroup Controls_Enums_Structs_Variables_Functions Controls File Constants, Enumerations, Structs, Shared Variables, and Functions
 * \brief Controls file constants, enumerations, structs, shared variables, and functions
 * \{
 */

/*!
 * \addtogroup Controls_Enums Controls File Enumerations
 * \brief Controls enumeration types.
 * \ingroup Controls_Enums_Structs_Variables_Functions
 * \{
 */
/*!
 * \enum RuleState
 * \brief Enumeration of rule interpreter states.
 */
enum RuleState
{
    /*! \brief Rule interpreter is idle */
    r_RULE,
    /*! \brief If clause */
    r_IF,
    /*! \brief And clause */
    r_AND,
    /*! \brief Or clause */
    r_OR,
    /*! \brief Then clause */
    r_THEN,
    /*! \brief Else clause */
    r_ELSE,
    /*! \brief Priority of control rule */
    r_PRIORITY,
    /*! \brief Variable definition */
    r_VARIABLE,
    /*! \brief Expression definition */
    r_EXPRESSION,
    /*! \brief Error */
    r_ERROR
};

/*!
 * \enum RuleObject
 * \brief Enumeration of rule object types.
 */
enum RuleObject
{
    /*! \brief Rain gage */
    r_GAGE,
    /*! \brief Node */
    r_NODE,
    /*! \brief Link */
    r_LINK,
    /*! \brief Conduit */
    r_CONDUIT,
    /*! \brief Pump */
    r_PUMP,
    /*! \brief Orifice */
    r_ORIFICE,
    /*! \brief Weir */
    r_WEIR,
    /*! \brief Outlet */
    r_OUTLET,
    /*! \brief Simulation */
    r_SIMULATION
};

/*!
 * \enum RuleAttrib
 * \brief Enumeration of rule attribute types.
 * \todo Might be useful to make all use same enum type and used in API to avoid duplication.
 */
enum RuleAttrib
{
    /*! \brief Depth */
    r_DEPTH,
    /*! \brief Maximum depth */
    r_MAXDEPTH,
    /*! \brief Head */
    r_HEAD,
    /*! \brief Volume */
    r_VOLUME,
    /*! \brief Inflow */
    r_INFLOW,
    /*! \brief Flow */
    r_FLOW,
    /*! \brief Full flow */
    r_FULLFLOW,
    /*! \brief Full depth */
    r_FULLDEPTH,
    /*! \brief Status */
    r_STATUS,
    /*! \brief Setting */
    r_SETTING,
    /*! \brief Length */
    r_LENGTH,
    /*! \brief Slope */
    r_SLOPE,
    /*! \brief Velocity */
    r_VELOCITY,
    /*! \brief Time open */
    r_TIMEOPEN,
    /*! \brief Time closed */
    r_TIMECLOSED,
    /*! \brief Time */
    r_TIME,
    /*! \brief Date */
    r_DATE,
    /*! \brief Clock time */
    r_CLOCKTIME,
    /*! \brief Day of year */
    r_DAYOFYEAR,
    /*! \brief Day */
    r_DAY,
    /*! \brief Month */
    r_MONTH
};

/*!
 * \enum RuleRelation
 * \brief Enumeration of rule relational operators.
 */
enum RuleRelation
{
    /*! \brief Equal */
    EQ,
    /*! \brief Not equal */
    NE,
    /*! \brief Less than */
    LT,
    /*! \brief Less than or equal */
    LE,
    /*! \brief Greater than */
    GT,
    /*! \brief Greater than or equal */
    GE
};

/*!
 * \enum RuleSetting
 * \brief Enumeration of rule setting types.
 */
enum RuleSetting
{   
    /*! \brief Curve */
    r_CURVE,
    /*! \brief Timeseries */
    r_TIMESERIES,
    /*! \brief PID */
    r_PID,
    /*! \brief Numeric */
    r_NUMERIC
};

/*!
 * \}
 */

/*!
 * \addtogroup Controls_Vars Controls File Variables
 * \brief Controls file variables and definitions.
 * \ingroup Controls_Enums_Structs_Variables_Functions
 * \{
 */

/*!
 * \def MAXVARNAME
 * \brief Maximum length of a variable name
 */
#define MAXVARNAME 32

/*!
 * \var ObjectWords
 * \brief Rule object words
 */
static char *ObjectWords[] = {
    "GAGE",
    "NODE",
    "LINK",
    "CONDUIT",
    "PUMP",
    "ORIFICE",
    "WEIR",
    "OUTLET",
    "SIMULATION",
    NULL
};

/*!
 * \var AttribWords
 * \brief Rule attribute words
 */
static char *AttribWords[] = {
    "DEPTH",
    "MAXDEPTH",
    "HEAD",
    "VOLUME",
    "INFLOW",
    "FLOW",
    "FULLFLOW",
    "FULLDEPTH",
    "STATUS",
    "SETTING",
    "LENGTH",
    "SLOPE",
    "VELOCITY",
    "TIMEOPEN",
    "TIMECLOSED",
    "TIME",
    "DATE",
    "CLOCKTIME",
    "DAYOFYEAR",
    "DAY",
    "MONTH",
    NULL
};

/*!
 * \var RelOpWords
 * \brief Rule relational operator words
 */
static char *RelOpWords[] = {"=", "<>", "<", "<=", ">", ">=", NULL};

/*!
 * \var StatusWords
 * \brief Rule status words
 */
static char *StatusWords[] = {"OFF", "ON", NULL};

/*!
 * \var ConduitWords
 * \brief Rule conduit status words
 */
static char *ConduitWords[] = {"CLOSED", "OPEN", NULL};

/*!
 * \var SettingTypeWords
 * \brief Rule setting type words
 */
static char *SettingTypeWords[] = {"CURVE", "TIMESERIES", "PID", NULL};

/*!
 * \var IntensityWord
 * \brief Rule intensity word
 */
static char *IntensityWord = "INTENSITY";

/*!
 * \}
 */

/*!
 * \addtogroup Controls_Structs Controls File Structures
 * \brief Controls structures.
 * \ingroup Controls_Enums_Structs_Variables_Functions
 * \{
 */

/*!
 * \struct TVariable
 * \brief Rule premise variable.
 */
struct TVariable
{
    /*! \brief Type of object */
    int object;
    /*! \brief Index in object's array */
    int index;
    /*! \brief Object's attribute */
    int attribute;
};

/*!
 * \struct TNamedVariable
 * \brief Rule premise named variable.
 */
struct TNamedVariable
{
    /*! \brief Rule premise variable */
    struct TVariable variable; // a rule premise variable

    /*! \brief Name used in math expression */
    char name[MAXVARNAME + 1]; // name used in math expression
};

/*!
 * \struct TExpression
 * \brief Rule premise math expression.
 */
struct TExpression
{
    /*! \brief Tokenized math expression */
    MathExpr *expression; // tokenized math expression

    /*! \brief Name of expression */
    char name[MAXVARNAME + 1]; // expression name
};

/*!
 * \struct TPremise
 * \brief Rule premise clause.
 */
struct TPremise
{
    /*! \brief Clause type (IF/AND/OR) */
    int type; // clause type (IF/AND/OR)

    /*! \brief Index of expression (-1 if N/A) */
    int exprIndex; // expression index (-1 if N/A)

    /*! \brief Left hand side variable */
    struct TVariable lhsVar; // left hand side variable

    /*! \brief Right hand side variable */
    struct TVariable rhsVar; // right hand side variable

    /*! \brief Relational operator (>, <, =, etc) */
    int relation; // relational operator (>, <, =, etc)

    /*! \brief Right hand side value */
    double value; // right hand side value

    /*! \brief Next premise clause of rule */
    struct TPremise *next; // next premise clause of rule
};

/*!
 * \struct TAction
 * \brief Rule action clause.
 */
struct TAction
{
    /*! \brief Index of rule that action belongs to */
    int rule; // index of rule that action belongs to

    /*! link index */
    int link; // index of link being controlled

    /*! \brief Attribute of link being controlled */
    int attribute; // attribute of link being controlled

    /*! \brief Index of curve for modulated control */
    int curve; // index of curve for modulated control

    /*! \brief Index of time series for modulated control */
    int tseries; // index of time series for modulated control

    /*! \brief Value of control setting */
    double value; // control setting for link attribute

    /*! \brief PID coefficient Kp */
    double kp;

    /*! \brief PID coefficient Ki */
    double ki;

    /*! \brief PID coefficient Kd */
    double kd; // coeffs. for PID modulated control

    /*! \brief PID set point error from previous time steps */
    double e1;

    /*! \brief PID set point error from previous time steps */
    double e2; // PID set point error from previous time steps

    /*! \brief Next action clause of rule */
    struct TAction *next; // next action clause of rule
};

/*!
 * \struct TActionList
 * \brief Linked list of control actions.
 */
struct TActionList
{
    /*! \brief Action clause */
    struct TAction *action;

    /*! \brief Next action in list */
    struct TActionList *next;
};

/*!
 * \struct TRule
 * \brief Control rule.
 */
struct TRule
{
    /*! \brief Rule ID */
    char *ID; // rule ID
    /*! \brief Priority level */
    double priority; // priority level
    /*! \brief Pointer to first premise of rule */
    struct TPremise *firstPremise; // pointer to first premise of rule
    /*! \brief Pointer to last premise of rule */
    struct TPremise *lastPremise; // pointer to last premise of rule
    /*! \brief Pointer to actions if true */
    struct TAction *thenActions; // linked list of actions if true
    /*! \brief Pointer to actions if false */
    struct TAction *elseActions; // linked list of actions if false
};
/*!
 * \}
 */

//-----------------------------------------------------------------------------
//  Shared variables
//-----------------------------------------------------------------------------
/*!
 * \addtogroup Controls_Vars Controls File Variables
 * \brief Controls file variables and definitions.
 * \ingroup Controls_Enums_Structs_Variables_Functions
 * \{
 */

/*!
 * \var Rules
 * \brief Array of control rules.
 */
struct TRule *Rules;            // array of control rules

/*!
 * \var ActionList
 * \brief Linked list of control actions.
 */
struct TActionList *ActionList; // linked list of control actions

/*!
 * \var InputState
 * \brief State of rule interpreter.
 */
int InputState;                 // state of rule interpreter

/*!
 * \var RuleCount
 * \brief Total number of rules.
 */
int RuleCount;                  // total number of rules

/*!
 * \var ControlValue
 * \brief Value of controller variable.
 */
double ControlValue;            // value of controller variable

/*!
 * \var SetPoint
 * \brief Value of controller setpoint.
 */
double SetPoint;                // value of controller setpoint

/*!
 * \var CurrentDate
 * \brief Current date in whole days.
 */
DateTime CurrentDate;           // current date in whole days

/*!
 * \var CurrentTime
 * \brief Current time of day (decimal).
 */
DateTime CurrentTime;           // current time of day (decimal)

/*!
 * \var VariableCount
 * \brief Number of named variables.
 */
int VariableCount;

/*!
 * \var ExpressionCount
 * \brief Number of math expressions.
 */
int ExpressionCount;

/*!
 * \var CurrentVariable
 * \brief Index of current named variable.
 */
int CurrentVariable;

/*!
 * \var CurrentExpression
 * \brief Index of current math expression.
 */
int CurrentExpression;

/*!
 * \var NamedVariable
 * \brief Array of named variables.
 */
struct TNamedVariable *NamedVariable; // array of named variables

/*!
 * \var Expression
 * \brief Array of math expressions.
 */
struct TExpression *Expression;       // array of math expressions
/*!
 * \}
 */

//-----------------------------------------------------------------------------
//  External functions (declared in funcs.h)
//-----------------------------------------------------------------------------
//     controls_create
//     controls_delete
//     controls_init
//     controls_addToCount
//     controls_addVariable
//     controls_addExpression
//     controls_addRuleClause
//     controls_evaluate

//-----------------------------------------------------------------------------
//  Local functions
//-----------------------------------------------------------------------------
/*!
 * \addtogroup Controls_Functions Controls File Functions
 * \brief Controls file local functions.
 * \ingroup Controls_Enums_Structs_Variables_Functions
 * \{
 */

/*!
 * \brief Adds a premise clause to a control rule.
 * \param[in] r Rule index
 * \param[in] type Premise type
 * \param[in] Tok Array of string tokens
 * \param[in] nToks Number of tokens
 * \return Error code
 */
int addPremise(int r, int type, char *Tok[], int nToks);

/*!
 * \brief Retrieves a variable from a tokenized line of input.
 * \param[in] tok Array of string tokens
 * \param[in] nToks Number of tokens
 * \param[in] k Index of current token
 * \param[in, out] v Variable structure
 * \return Error code
 */
int getPremiseVariable(char *tok[], int nToks, int *k, struct TVariable *v);

/*!
 * \brief Retrieves a premise value from a tokenized line of input.
 * \param[in] token Token to evaluate
 * \param[in] attrib Attribute of object
 * \param[out] value Value of object
 * \return Error code
 */
int getPremiseValue(char *token, int attrib, double *value);

/*!
 * \brief Adds an action clause to a control rule.
 * \param[in] r Rule index
 * \param[in] Tok Array of string tokens
 * \param[in] nToks Number of tokens
 * \return Error code
 */
int addAction(int r, char *Tok[], int nToks);

/*!
 * \brief Evaluates premise clause of a control rule.
 * \param[in] p Premise structure to evaluate
 * \param[in] tStep Time step
 * \return TRUE if premise is true, FALSE if not
 */
int evaluatePremise(struct TPremise *p, double tStep);

/*!
 * \brief Gets the value of a variable.
 * \param[in] v Variable structure
 * \return Value of variable
 */
double getVariableValue(struct TVariable v);

/*!
 * \brief Compares two times.
 * \param[in] lhsValue Left hand side value
 * \param[in] relation Relation operator
 * \param[in] rhsValue Right hand side value
 * \param[in] halfStep Half time step
 * \return TRUE if relation is true, FALSE if not
 */
int compareTimes(double lhsValue, int relation, double rhsValue, double halfStep);

/*!
 * \brief Compares two values.
 * \param[in] lhsValue Left hand side value
 * \param[in] relation Relation operator
 * \param[in] rhsValue Right hand side value
 * \return TRUE if relation is true, FALSE if not
 */
int compareValues(double lhsValue, int relation, double rhsValue);

/*!
 * \brief Updates action list.
 * \param[in] a Action structure
 */
void updateActionList(struct TAction *a);

/*!
 * \brief Executes action list.
 * \param[in] currentTime Current time
 * \return Error code
 */
int executeActionList(DateTime currentTime);

/*!
 * \brief Clears action list.
 */
void clearActionList(void);

/*!
 * \brief Deletes action list.
 */
void deleteActionList(void);

/*!
 * \brief Deletes control rules.
 */
void deleteRules(void);

/*!
 * \brief Finds an exact match in a list of keywords.
 * \param[in] s String to match
 * \param[in] keyword Array of keywords
 * \return Index of keyword if found, -1 if not
 */
int findExactMatch(char *s, char *keyword[]);

/*!
 * \brief Sets the value of a control action.
 * \param[in] tok Array of string tokens
 * \param[in] nToks Number of tokens
 * \param[in] curve Curve index
 * \param[in] tseries Time series index
 * \param[in] attrib Attribute index
 * \param[out] value Value of action
 * \return Error code
 */
int setActionSetting(char *tok[], int nToks, int *curve, int *tseries,
                     int *attrib, double value[]);

/*!
* \brief Updates the value of a control action.
* \param[in] a Action structure
* \param[in] currentTime Current time
* \param[in] dt Time step
*/
void updateActionValue(struct TAction *a, DateTime currentTime, double dt);

/*!
 * \brief Gets PID setting.
 * \param[in] a Action structure
 * \param[in] dt Time step
 * \return Value of action
 */
double getPIDSetting(struct TAction *a, double dt);

/*!
 * \brief Gets variable index.
 * \param[in] varName Name of variable to find index of
 * \return Index of variable 
 */
int getVariableIndex(char *varName);

/*!
 * \brief Gets named variable value.
 * \param[in] varIndex Index of variable
 * \return Value of variable
 */
double getNamedVariableValue(int varIndex);

/*!
 * \brief Gets expression index.
 * \param[in] exprName Name of expression to find index of
 * \return Index of expression
 */
int getExpressionIndex(char *exprName);

/*!
 * \brief Gets gauge attribute.
 * \param[in] token Token to evaluate
 * \return Attribute of gauge
 */
int getGageAttrib(char *token);

/*!
 * \brief Gets rain value.
 * \param v Variable structure
 */
double getRainValue(struct TVariable v);
/*!
 * \}
 */

/*!
 * \}
 */

/*!
 * \brief Initializes the control rule system.
 */
void controls_init()
{
    Rules = NULL;
    NamedVariable = NULL;
    Expression = NULL;
    RuleCount = 0;
    VariableCount = 0;
    ExpressionCount = 0;
}

/*!
* \brief Updates the number of named variables or math expressions used
* by control rules.
* \param[in] s String containing either "VARIABLE" or "EXPRESSION"
*/
void controls_addToCount(char *s)
{
    if (match(s, w_VARIABLE))
        VariableCount++;
    else if (match(s, w_EXPRESSION))
        ExpressionCount++;
}

/*!
 * \brief Creates an array of control rules.
 * \param[in] n Total number of control rules
 * \return Error code
 */
int controls_create(int n)
{
    int r;
    ActionList = NULL;
    InputState = r_PRIORITY;
    RuleCount = n;
    if (RuleCount > 0)
    {
        Rules = (struct TRule *)calloc(RuleCount, sizeof(struct TRule));
        if (Rules == NULL)
            return ERR_MEMORY;
        for (r = 0; r < RuleCount; r++)
        {
            Rules[r].ID = NULL;
            Rules[r].firstPremise = NULL;
            Rules[r].lastPremise = NULL;
            Rules[r].thenActions = NULL;
            Rules[r].elseActions = NULL;
            Rules[r].priority = 0.0;
        }
    }

    CurrentVariable = -1;
    CurrentExpression = -1;
    if (VariableCount > 0)
    {
        NamedVariable = (struct TNamedVariable *)calloc(VariableCount,
                                                        sizeof(struct TNamedVariable));
        if (NamedVariable == NULL)
            return ERR_MEMORY;
    }
    if (ExpressionCount > 0)
    {
        Expression = (struct TExpression *)calloc(ExpressionCount,
                                                  sizeof(struct TExpression));
        if (Expression == NULL)
            return ERR_MEMORY;
    }
    return 0;
}

/*!
 * \brief Deletes all control rules.
 */
void controls_delete(void)
{
    int i;

    for (i = 0; i < ExpressionCount; i++)
    {
        mathexpr_delete(Expression[i].expression);
        Expression[i].expression = NULL;
    }
    FREE(Expression);
    FREE(NamedVariable);

    if (RuleCount == 0)
        return;
    deleteActionList();
    deleteRules();
}

/*!
* \brief Adds a named variable to the control rule system from a
* tokenized line of input with formats:
*  - VARIABLE  name = Object  id  attribute
*  - VARIABLE  name = SIMULATION attribute
* \param[in] tok Array of string tokens
* \param[in] nToks Number of tokens
* \return Error code
*/
int controls_addVariable(char *tok[], int nToks)
{
    struct TVariable v1;
    int k, err;

    CurrentVariable++;
    if (nToks < 5)
        return ERR_ITEMS;
    if (findExactMatch(tok[1], AttribWords) >= 0)
        return error_setInpError(ERR_KEYWORD, tok[1]);
    if (!match(tok[2], "="))
        return error_setInpError(ERR_KEYWORD, tok[2]);
    if (!match(tok[3], "SIMULATION") && nToks < 6)
        return ERR_ITEMS;
    k = 3;
    err = getPremiseVariable(tok, nToks, &k, &v1);
    if (err > 0)
        return err;
    k = CurrentVariable;
    NamedVariable[k].variable = v1;
    sstrncpy(NamedVariable[k].name, tok[1], MAXVARNAME);
    return 0;
}

/*!
* \brief Adds a math expression to the control rule system from a
* a tokenized line of input with format:
*   EXPRESSION  name = <math expression containing VARIABLE names>
* \param[in] tok Array of string tokens
* \param[in] nToks Number of tokens
* \return Error code
*/
int controls_addExpression(char *tok[], int nToks)
{
    int i, k;
    char s[MAXLINE + 1];
    MathExpr *expr;

    CurrentExpression++;
    if (nToks < 4)
        return ERR_ITEMS;
    k = CurrentExpression;
    Expression[k].expression = NULL;
    sstrncpy(Expression[k].name, tok[1], MAXVARNAME);
    sstrncpy(s, tok[3], MAXLINE);
    for (i = 4; i < nToks; i++)
    {
        sstrcat(s, " ", MAXLINE);
        sstrcat(s, tok[i], MAXLINE);
    }

    expr = mathexpr_create(s, getVariableIndex);
    if (expr == NULL)
        return error_setInpError(ERR_MATH_EXPR, "");

    Expression[k].expression = expr;
    return 0;
}

/*!
* \brief Finds the array index of a named variable.
* \param[in] varName String containing a variable name
* \return Index of the named variable or -1 if not found
*/
int getVariableIndex(char *varName)
{
    int i;
    for (i = 0; i < VariableCount; i++)
    {
        if (match(varName, NamedVariable[i].name))
            return i;
    }
    return -1;
}

/*!
* \brief Finds the value of a named variable.
* \param[in] varIndex Index of a named variable
* \return Current value of the variable
*/
double getNamedVariableValue(int varIndex)
{
    return getVariableValue(NamedVariable[varIndex].variable);
}

/*!
* \brief Finds the array index of a math expression.
* \param[in] exprName String containing an expression name
* \return Index of the expression or -1 if not found
*/
int getExpressionIndex(char *exprName)
{
    int i;
    for (i = 0; i < ExpressionCount; i++)
    {
        if (match(exprName, Expression[i].name))
            return i;
    }
    return -1;
}

/*!
* \brief Adds a new clause to a control rule.
* \param[in] r Rule index
* \param[in] keyword Clause's keyword code (IF, THEN, etc.)
* \param[in] tok Array of string tokens that comprises the clause
* \param[in] nToks Number of tokens
* \return Error code
*/
int controls_addRuleClause(int r, int keyword, char *tok[], int nToks)
{
    switch (keyword)
    {
    case r_RULE:
        if (Rules[r].ID == NULL)
            Rules[r].ID = project_findID(CONTROL, tok[1]);
        InputState = r_RULE;
        if (nToks > 2)
            return ERR_RULE;
        return 0;

    case r_IF:
        if (InputState != r_RULE)
            return ERR_RULE;
        InputState = r_IF;
        return addPremise(r, r_AND, tok, nToks);

    case r_AND:
        if (InputState == r_IF)
            return addPremise(r, r_AND, tok, nToks);
        else if (InputState == r_THEN || InputState == r_ELSE)
            return addAction(r, tok, nToks);
        else
            return ERR_RULE;

    case r_OR:
        if (InputState != r_IF)
            return ERR_RULE;
        return addPremise(r, r_OR, tok, nToks);

    case r_THEN:
        if (InputState != r_IF)
            return ERR_RULE;
        InputState = r_THEN;
        return addAction(r, tok, nToks);

    case r_ELSE:
        if (InputState != r_THEN)
            return ERR_RULE;
        InputState = r_ELSE;
        return addAction(r, tok, nToks);

    case r_PRIORITY:
        if (InputState != r_THEN && InputState != r_ELSE)
            return ERR_RULE;
        InputState = r_PRIORITY;
        if (!getDouble(tok[1], &Rules[r].priority))
            return ERR_NUMBER;
        if (nToks > 2)
            return ERR_RULE;
        return 0;
    }
    return 0;
}

/*!
* \brief Evaluates all control rules at the current time of the simulation.
* \param[in] currentTime Current simulation date/time
* \param[in] elapsedTime Decimal days since start of simulation
* \param[in] tStep Simulation time step (days)
* \return Number of new actions taken
*/
int controls_evaluate(DateTime currentTime, DateTime elapsedTime, double tStep)
{
    int r;              // control rule index
    int result;         // TRUE if rule premises satisfied
    struct TPremise *p; // pointer to rule premise clause
    struct TAction *a;  // pointer to rule action clause

    // --- save date and time to shared variables
    CurrentDate = floor(currentTime);
    CurrentTime = currentTime - floor(currentTime);
    ElapsedTime = elapsedTime;

    // --- evaluate each rule
    if (RuleCount == 0)
        return 0;
    clearActionList();
    for (r = 0; r < RuleCount; r++)
    {
        // --- evaluate rule's premises
        result = TRUE;
        p = Rules[r].firstPremise;
        while (p)
        {
            if (p->type == r_OR)
            {
                if (result == FALSE)
                    result = evaluatePremise(p, tStep);
            }
            else
            {
                if (result == FALSE)
                    break;
                result = evaluatePremise(p, tStep);
            }
            p = p->next;
        }

        // --- if premises true, add THEN clauses to action list
        //     else add ELSE clauses to action list
        if (result == TRUE)
            a = Rules[r].thenActions;
        else
            a = Rules[r].elseActions;
        while (a)
        {
            updateActionValue(a, currentTime, tStep);
            updateActionList(a);
            a = a->next;
        }
    }

    // --- execute actions on action list
    if (ActionList)
        return executeActionList(currentTime);
    else
        return 0;
}

/*!
* \brief Adds a new premise to a control rule.
* \param[in] r Control rule index
* \param[in] type Type of premise (IF, AND, OR)
* \param[in] tok Array of string tokens containing premise statement
* \param[in] nToks Number of string tokens
* \return Error code
*/
int addPremise(int r, int type, char *tok[], int nToks)
{
    int relation, n, err = 0;
    double value = MISSING;
    struct TPremise *p;
    struct TVariable v1;
    struct TVariable v2;
    int obj, exprIndex, varIndex = -1;

    // --- initialize LHS variable v1
    if (nToks < 4)
        return ERR_ITEMS;
    v1.attribute = -1;
    v1.object = -1;
    v1.index = -1;
    n = 1;

    // --- check if 2nd token is a math expression
    exprIndex = getExpressionIndex(tok[1]);

    // --- if not then check if it's a named variable
    if (exprIndex < 0)
    {
        varIndex = getVariableIndex(tok[n]);
        if (varIndex >= 0)
        {
            v1 = NamedVariable[varIndex].variable;
        }

        // otherwise parse object|index|attribute tokens
        else
        {
            err = getPremiseVariable(tok, nToks, &n, &v1);
            if (err > 0)
                return err;
        }
    }

    // --- get relational operator
    n++;
    if (n >= nToks)
        return error_setInpError(ERR_ITEMS, "");
    relation = findExactMatch(tok[n], RelOpWords);
    if (relation < 0)
        return error_setInpError(ERR_KEYWORD, tok[n]);

    // --- initialize RHS variable v2
    v2.attribute = -1;
    v2.object = -1;
    v2.index = -1;
    n++;
    if (n >= nToks)
        return error_setInpError(ERR_ITEMS, "");

    // --- check for named RHS variable
    varIndex = getVariableIndex(tok[n]);
    if (varIndex >= 0)
    {
        v2 = NamedVariable[varIndex].variable;
    }

    // --- check for object|index|attribute variable
    else
    {
        obj = findmatch(tok[n], ObjectWords);
        if (obj >= 0)
        {
            err = getPremiseVariable(tok, nToks, &n, &v2);
            if (err > 0)
                return ERR_RULE;
            if (exprIndex < 0 && v1.attribute != v2.attribute)
                report_writeWarningMsg(WARN11, Rules[r].ID);
        }

        // --- check for a single RHS value
        else
        {
            err = getPremiseValue(tok[n], v1.attribute, &value);
            if (err > 0)
                return err;
        }
    }

    // --- make sure another clause is not on same line
    n++;
    if (n < nToks && findmatch(tok[n], RuleKeyWords) >= 0)
        return ERR_RULE;

    // --- create the premise object
    p = (struct TPremise *)malloc(sizeof(struct TPremise));
    if (!p)
        return ERR_MEMORY;
    p->type = type;
    p->exprIndex = exprIndex;
    p->lhsVar = v1;
    p->rhsVar = v2;
    p->relation = relation;
    p->value = value;
    p->next = NULL;
    if (Rules[r].firstPremise == NULL)
    {
        Rules[r].firstPremise = p;
    }
    else
    {
        Rules[r].lastPremise->next = p;
    }
    Rules[r].lastPremise = p;
    return 0;
}

/*!
* \brief Parses a variable (e.g., Node 123 Depth) used in a control rule.
* \param[in] tok Array of string tokens
* \param[in] nToks Number of tokens
* \param[in] k Index of current token
* \param[in, out] v Variable structure
* \return Error code; updates k to new current token and places identity of specified variable in v
*/
int getPremiseVariable(char *tok[], int nToks, int *k, struct TVariable *v)
{
    int n = *k;
    int object = -1;
    int index = -1;
    int obj, attrib;

    // --- get object type
    obj = findmatch(tok[n], ObjectWords);
    if (obj < 0)
        return error_setInpError(ERR_KEYWORD, tok[n]);

    // --- get object index from its name
    n++;
    if (n >= nToks)
        return error_setInpError(ERR_ITEMS, "");
    switch (obj)
    {
    case r_GAGE:
        index = project_findObject(GAGE, tok[n]);
        if (index < 0)
            return error_setInpError(ERR_NAME, tok[n]);
        object = r_GAGE;
        break;

    case r_NODE:
        index = project_findObject(NODE, tok[n]);
        if (index < 0)
            return error_setInpError(ERR_NAME, tok[n]);
        object = r_NODE;
        break;

    case r_LINK:
    case r_CONDUIT:
    case r_PUMP:
    case r_ORIFICE:
    case r_WEIR:
    case r_OUTLET:
        index = project_findObject(LINK, tok[n]);
        if (index < 0)
            return error_setInpError(ERR_NAME, tok[n]);
        object = r_LINK;
        break;
    default:
        n--;
    }
    n++;
    if (n >= nToks)
        return error_setInpError(ERR_ITEMS, "");

    // --- get attribute index from its name
    if (object == r_GAGE)
        attrib = getGageAttrib(tok[n]);
    else
        attrib = findmatch(tok[n], AttribWords);
    if (attrib < 0)
        return error_setInpError(ERR_KEYWORD, tok[n]);

    // --- check that attribute belongs to object type
    if (obj == r_GAGE)
    {
    }

    else if (obj == r_NODE)
        switch (attrib)
        {
        case r_DEPTH:
        case r_MAXDEPTH:
        case r_HEAD:
        case r_VOLUME:
        case r_INFLOW:
            break;
        default:
            return error_setInpError(ERR_KEYWORD, tok[n]);
        }

    // --- check for link TIMEOPEN & TIMECLOSED attributes
    else if (object == r_LINK && index >= 0 &&
             ((attrib == r_TIMEOPEN || attrib == r_TIMECLOSED)))
    {
        // nothing to do here
    }

    else if (obj == r_LINK || obj == r_CONDUIT)
        switch (attrib)
        {
        case r_STATUS:
        case r_DEPTH:
        case r_FULLFLOW:
        case r_FULLDEPTH:
        case r_FLOW:
        case r_LENGTH:
        case r_SLOPE:
        case r_VELOCITY:
            break;
        default:
            return error_setInpError(ERR_KEYWORD, tok[n]);
        }
    else if (obj == r_PUMP)
        switch (attrib)
        {
        case r_FLOW:
        case r_SETTING:
        case r_STATUS:
            break;
        default:
            return error_setInpError(ERR_KEYWORD, tok[n]);
        }
    else if (obj == r_ORIFICE || obj == r_WEIR ||
             obj == r_OUTLET)
        switch (attrib)
        {
        case r_FLOW:
        case r_SETTING:
            break;
        default:
            return error_setInpError(ERR_KEYWORD, tok[n]);
        }
    else
        switch (attrib)
        {
        case r_TIME:
        case r_DATE:
        case r_CLOCKTIME:
        case r_DAY:
        case r_MONTH:
        case r_DAYOFYEAR:
            break;
        default:
            return error_setInpError(ERR_KEYWORD, tok[n]);
        }

    // --- populate variable structure
    v->object = object;
    v->index = index;
    v->attribute = attrib;
    *k = n;
    return 0;
}

/*!
* \brief Determines the attribute code for a rain gage variable.
* \param[in] token String token
* \return Attribute code or -1 if an error occurred
* \note A valid token is INTENSITY for current rainfall intensity (attribute code = 0) 
* or nHR_PRECIP for total rain depth over past n hours (attribute code = n).
*/
int getGageAttrib(char *token)
{
    int attrib;

    // --- check if token is currrent rainfall intensity
    if (match(token, IntensityWord))
        return 0;

    // --- token is past rain depth - read number of past hours
    attrib = atoi(token);

    // --- check that number of hours is in allowable range
    if (attrib < 1 || attrib > MAXPASTRAIN)
        return -1;
    return attrib;
}

/*!
* \brief Parses the numerical value of a particular node/link attribute
* in the premise clause of a control rule.
* \param[in] token String token
* \param[in] attrib Index of a node/link attribute
* \param[out] value Attribute value
* \return Error code
*/
int getPremiseValue(char *token, int attrib, double *value)
{
    char strDate[25];
    switch (attrib)
    {
    case r_STATUS:
        *value = findmatch(token, StatusWords);
        if (*value < 0.0)
            *value = findmatch(token, ConduitWords);
        if (*value < 0.0)
            return error_setInpError(ERR_KEYWORD, token);
        break;

    case r_TIME:
    case r_CLOCKTIME:
    case r_TIMEOPEN:
    case r_TIMECLOSED:
        if (!datetime_strToTime(token, value))
            return error_setInpError(ERR_DATETIME, token);
        break;

    case r_DATE:
        if (!datetime_strToDate(token, value))
            return error_setInpError(ERR_DATETIME, token);
        break;

    case r_DAY:
        if (!getDouble(token, value))
            return error_setInpError(ERR_NUMBER, token);
        if (*value < 1.0 || *value > 7.0)
            return error_setInpError(ERR_DATETIME, token);
        break;

    case r_MONTH:
        if (!getDouble(token, value))
            return error_setInpError(ERR_NUMBER, token);
        if (*value < 1.0 || *value > 12.0)
            return error_setInpError(ERR_DATETIME, token);
        break;

    case r_DAYOFYEAR:
        sstrncpy(strDate, token, 6);
        sstrcat(strDate, "/1947", 25);
        if (datetime_strToDate(strDate, value))
        {
            *value = datetime_dayOfYear(*value);
        }
        else if (!getDouble(token, value) || *value < 1 || *value > 365)
            return error_setInpError(ERR_DATETIME, token);
        break;

    default:
        if (!getDouble(token, value))
            return error_setInpError(ERR_NUMBER, token);
    }
    return 0;
}

/*!
* \brief Adds a new action to a control rule.
* \param[in] r Rule index
* \param[in] tok Array of string tokens containing action statement
* \param[in] nToks Number of string tokens
* \return Error code
*/
int addAction(int r, char *tok[], int nToks)
{
    int obj, link, attrib;
    int curve = -1, tseries = -1;
    int n;
    int err;
    double values[] = {1.0, 0.0, 0.0};

    struct TAction *a;

    // --- check for proper number of tokens
    if (nToks < 6)
        return error_setInpError(ERR_ITEMS, "");

    // --- check for valid object type
    obj = findmatch(tok[1], ObjectWords);
    if (obj != r_LINK && obj != r_CONDUIT && obj != r_PUMP &&
        obj != r_ORIFICE && obj != r_WEIR && obj != r_OUTLET)
        return error_setInpError(ERR_KEYWORD, tok[1]);

    // --- check that object name exists and is of correct type
    link = project_findObject(LINK, tok[2]);
    if (link < 0)
        return error_setInpError(ERR_NAME, tok[2]);
    switch (obj)
    {
    case r_CONDUIT:
        if (Link[link].type != CONDUIT)
            return error_setInpError(ERR_NAME, tok[2]);
        break;
    case r_PUMP:
        if (Link[link].type != PUMP)
            return error_setInpError(ERR_NAME, tok[2]);
        break;
    case r_ORIFICE:
        if (Link[link].type != ORIFICE)
            return error_setInpError(ERR_NAME, tok[2]);
        break;
    case r_WEIR:
        if (Link[link].type != WEIR)
            return error_setInpError(ERR_NAME, tok[2]);
        break;
    case r_OUTLET:
        if (Link[link].type != OUTLET)
            return error_setInpError(ERR_NAME, tok[2]);
        break;
    }

    // --- check for valid attribute name
    attrib = findmatch(tok[3], AttribWords);
    if (attrib < 0)
        return error_setInpError(ERR_KEYWORD, tok[3]);

    // --- get control action setting
    if (obj == r_CONDUIT)
    {
        if (attrib == r_STATUS)
        {
            values[0] = findmatch(tok[5], ConduitWords);
            if (values[0] < 0.0)
                return error_setInpError(ERR_KEYWORD, tok[5]);
        }
        else
            return error_setInpError(ERR_KEYWORD, tok[3]);
    }

    else if (obj == r_PUMP)
    {
        if (attrib == r_STATUS)
        {
            values[0] = findmatch(tok[5], StatusWords);
            if (values[0] < 0.0)
                return error_setInpError(ERR_KEYWORD, tok[5]);
        }
        else if (attrib == r_SETTING)
        {
            err = setActionSetting(tok, nToks, &curve, &tseries,
                                   &attrib, values);
            if (err > 0)
                return err;
        }
        else
            return error_setInpError(ERR_KEYWORD, tok[3]);
    }

    else if (obj == r_ORIFICE || obj == r_WEIR || obj == r_OUTLET)
    {
        if (attrib == r_SETTING)
        {
            err = setActionSetting(tok, nToks, &curve, &tseries,
                                   &attrib, values);
            if (err > 0)
                return err;
            if (attrib == r_SETTING && (values[0] < 0.0 || values[0] > 1.0))
                return error_setInpError(ERR_NUMBER, tok[5]);
        }
        else
            return error_setInpError(ERR_KEYWORD, tok[3]);
    }
    else
        return error_setInpError(ERR_KEYWORD, tok[1]);

    // --- check if another clause is on same line
    n = 6;
    if (curve >= 0 || tseries >= 0)
        n = 7;
    if (attrib == r_PID)
        n = 9;
    if (n < nToks && findmatch(tok[n], RuleKeyWords) >= 0)
        return ERR_RULE;

    // --- create the action object
    a = (struct TAction *)malloc(sizeof(struct TAction));
    if (!a)
        return ERR_MEMORY;
    a->rule = r;
    a->link = link;
    a->attribute = attrib;
    a->curve = curve;
    a->tseries = tseries;
    a->value = values[0];
    if (attrib == r_PID)
    {
        a->kp = values[0];
        a->ki = values[1];
        a->kd = values[2];
        a->e1 = 0.0;
        a->e2 = 0.0;
    }
    if (InputState == r_THEN)
    {
        a->next = Rules[r].thenActions;
        Rules[r].thenActions = a;
    }
    else
    {
        a->next = Rules[r].elseActions;
        Rules[r].elseActions = a;
    }
    return 0;
}

/*!
* \brief Identifies how control actions settings are determined.
* \param[in] tok Array of string tokens containing action statement
* \param[in] nToks Number of string tokens
* \param[in] curve Index of controller curve
* \param[in] tseries Index of controller time series
* \param[in] attrib r_PID if PID controller used
* \param[out] values Values of control settings
* \return Error code
*/
int setActionSetting(char *tok[], int nToks, int *curve, int *tseries,
                     int *attrib, double values[])
{
    int k, m;

    // --- see if control action is determined by a Curve or Time Series
    if (nToks < 6)
        return error_setInpError(ERR_ITEMS, "");
    k = findmatch(tok[5], SettingTypeWords);
    if (k >= 0 && nToks < 7)
        return error_setInpError(ERR_ITEMS, "");
    switch (k)
    {

    // --- control determined by a curve - find curve index
    case r_CURVE:
        m = project_findObject(CURVE, tok[6]);
        if (m < 0)
            return error_setInpError(ERR_NAME, tok[6]);
        *curve = m;
        break;

    // --- control determined by a time series - find time series index
    case r_TIMESERIES:
        m = project_findObject(TSERIES, tok[6]);
        if (m < 0)
            return error_setInpError(ERR_NAME, tok[6]);
        *tseries = m;
        Tseries[m].refersTo = CONTROL;
        break;

    // --- control determined by PID controller
    case r_PID:
        if (nToks < 9)
            return error_setInpError(ERR_ITEMS, "");
        for (m = 6; m <= 8; m++)
        {
            if (!getDouble(tok[m], &values[m - 6]))
                return error_setInpError(ERR_NUMBER, tok[m]);
        }
        *attrib = r_PID;
        break;

    // --- direct numerical control is used
    default:
        if (!getDouble(tok[5], &values[0]))
            return error_setInpError(ERR_NUMBER, tok[5]);
    }
    return 0;
}

/*!
* \brief Updates the value of actions found from Curves or Time Series.
* \param[in] a Action structure
* \param[in] currentTime Current time of simulation in days (days)
* \param[in] dt Time step (days)
*/
void updateActionValue(struct TAction *a, DateTime currentTime, double dt)
//
//  Input:   a = an action object
//           currentTime = current simulation date/time (days)
//           dt = time step (days)
//  Output:  none
//  Purpose: updates value of actions found from Curves or Time Series.
//
{
    if (a->curve >= 0)
    {
        a->value = table_lookup(&Curve[a->curve], ControlValue);
    }
    else if (a->tseries >= 0)
    {
        a->value = table_tseriesLookup(&Tseries[a->tseries], currentTime, TRUE);
    }
    else if (a->attribute == r_PID)
    {
        a->value = getPIDSetting(a, dt);
    }
}

/*!
* \brief Computes a new setting for a link subject to a PID controller.
* \param[in] a Action object
* \param[in] dt Current time step (days)
* \return New link setting value
* \note 
* - a->kp = gain coefficient,
* - a->ki = integral time (minutes),
* - a->k2 = derivative time (minutes),
* - a->e1 = error from previous time step,
* - a->e2 = error from two time steps ago
*/
double getPIDSetting(struct TAction *a, double dt)
{
    double e0, setting;
    double p, i, d, update;
    double tolerance = 0.0001;

    // --- convert time step from days to minutes
    dt *= 1440.0;

    // --- determine relative error in achieving controller set point
    e0 = SetPoint - ControlValue;
    if (fabs(e0) > TINY)
    {
        if (SetPoint != 0.0)
            e0 = e0 / SetPoint;
        else
            e0 = e0 / ControlValue;
    }

    // --- reset previous errors to 0 if controller gets stuck
    if (fabs(e0 - a->e1) < tolerance)
    {
        a->e2 = 0.0;
        a->e1 = 0.0;
    }

    // --- use the recursive form of the PID controller equation to
    //     determine the new setting for the controlled link
    p = (e0 - a->e1);
    if (a->ki == 0.0)
        i = 0.0;
    else
        i = e0 * dt / a->ki;
    d = a->kd * (e0 - 2.0 * a->e1 + a->e2) / dt;
    update = a->kp * (p + i + d);
    if (fabs(update) < tolerance)
        update = 0.0;
    setting = Link[a->link].targetSetting + update;

    // --- update previous errors
    a->e2 = a->e1;
    a->e1 = e0;

    // --- check that new setting lies within feasible limits
    if (setting < 0.0)
        setting = 0.0;
    if (Link[a->link].type != PUMP && setting > 1.0)
        setting = 1.0;
    return setting;
}

/*!
* \brief Adds a new action to the list of actions to be taken.
* \param[in] a Action structure
*/
void updateActionList(struct TAction *a)
{
    struct TActionList *listItem;
    struct TAction *a1;
    double priority = Rules[a->rule].priority;

    // --- check if link referred to in action is already listed
    listItem = ActionList;
    while (listItem)
    {
        a1 = listItem->action;
        if (!a1)
            break;
        if (a1->link == a->link)
        {
            // --- replace old action if new action has higher priority
            if (priority > Rules[a1->rule].priority)
                listItem->action = a;
            return;
        }
        listItem = listItem->next;
    }

    // --- action not listed so add it to ActionList                           //5.2.1
    if (!listItem)
    {
        listItem = (struct TActionList *)malloc(sizeof(struct TActionList));
        listItem->next = ActionList;
        ActionList = listItem;
    }
    listItem->action = a;
}

/*!
* \brief Executes all actions required by fired control rules.
* \param[in] currentTime Current simulation date/time
* \return Number of new actions taken
*/
int executeActionList(DateTime currentTime)
{
    struct TActionList *listItem;
    struct TActionList *nextItem;
    struct TAction *a1;
    int count = 0;

    listItem = ActionList;
    while (listItem)
    {
        a1 = listItem->action;
        if (!a1)
            break;
        if (a1->link >= 0)
        {
            if (Link[a1->link].targetSetting != a1->value)
            {
                Link[a1->link].targetSetting = a1->value;
                if (RptFlags.controls && a1->curve < 0 && a1->tseries < 0 && a1->attribute != r_PID)
                    report_writeControlAction(currentTime, Link[a1->link].ID,
                                              a1->value, Rules[a1->rule].ID);
                count++;
            }
        }
        nextItem = listItem->next;
        listItem = nextItem;
    }
    return count;
}

/*!
* \brief Evaluates the truth of a control rule premise condition.
* \param[in] p Control rule premise condition
* \param[in] tStep Current time step (days)
* \return TRUE if the condition is true, FALSE otherwise
*/
int evaluatePremise(struct TPremise *p, double tStep)
{
    double lhsValue, rhsValue;
    int result = FALSE;

    // --- check if left hand side (lhs) of premise is an expression
    if (p->exprIndex >= 0)
        lhsValue = mathexpr_eval(Expression[p->exprIndex].expression,
                                 getNamedVariableValue);

    // --- otherwise get value of the lhs variable
    else
        lhsValue = getVariableValue(p->lhsVar);

    // --- if right hand side (rhs) of premise is a variable then get its value
    if (p->value == MISSING)
        rhsValue = getVariableValue(p->rhsVar);
    else
        rhsValue = p->value;
    if (lhsValue == MISSING || rhsValue == MISSING)
        return FALSE;

    // --- compare the lhs of the premise to the rhs
    switch (p->lhsVar.attribute)
    {
    case r_TIME:
    case r_CLOCKTIME:
        return compareTimes(lhsValue, p->relation, rhsValue, tStep / 2.0);
    case r_TIMEOPEN:
    case r_TIMECLOSED:
        result = compareTimes(lhsValue, p->relation, rhsValue, tStep / 2.0);
        ControlValue = lhsValue * 24.0; // convert time from days to hours
        return result;
    default:
        return compareValues(lhsValue, p->relation, rhsValue);
    }
}

/*!
* \brief Gets variable value
* \param[in] v Variable structure
* \return Variable value
*/
double getVariableValue(struct TVariable v)
{
    int i = -1; // a node index
    int j = -1; // a link index

    if (v.object == r_GAGE)
        return getRainValue(v);
    if (v.object == r_NODE)
        i = v.index;
    if (v.object == r_LINK)
        j = v.index;

    switch (v.attribute)
    {
    case r_TIME:
        return ElapsedTime;

    case r_DATE:
        return CurrentDate;

    case r_CLOCKTIME:
        return CurrentTime;

    case r_DAY:
        return datetime_dayOfWeek(CurrentDate);

    case r_MONTH:
        return datetime_monthOfYear(CurrentDate);

    case r_DAYOFYEAR:
        return datetime_dayOfYear(CurrentDate);

    case r_STATUS:
        if (j < 0 ||
            (Link[j].type != CONDUIT && Link[j].type != PUMP))
            return MISSING;
        else
            return Link[j].setting;

    case r_SETTING:
        if (j < 0 || (Link[j].type != PUMP &&
                      Link[j].type != ORIFICE &&
                      Link[j].type != WEIR))
            return MISSING;
        else
            return Link[j].setting;

    case r_FLOW:
        if (j < 0)
            return MISSING;
        else
            return Link[j].direction * Link[j].newFlow * UCF(FLOW);

    case r_FULLFLOW:
    case r_FULLDEPTH:
    case r_VELOCITY:
    case r_LENGTH:
    case r_SLOPE:
        if (j < 0)
            return MISSING;
        else if (Link[j].type != CONDUIT)
            return MISSING;
        switch (v.attribute)
        {
        case r_FULLFLOW:
            return Link[j].qFull * UCF(FLOW);
        case r_FULLDEPTH:
            return Link[j].xsect.yFull * UCF(LENGTH);
        case r_VELOCITY:
            return link_getVelocity(j, Link[j].newFlow, Link[j].newDepth) * UCF(LENGTH);
        case r_LENGTH:
            return Conduit[Link[j].subIndex].length * UCF(LENGTH);
        case r_SLOPE:
            return Conduit[Link[j].subIndex].slope;
        default:
            return MISSING;
        }
    case r_DEPTH:
        if (j >= 0)
            return Link[j].newDepth * UCF(LENGTH);
        else if (i >= 0)
            return Node[i].newDepth * UCF(LENGTH);
        else
            return MISSING;

    case r_MAXDEPTH:
        if (i >= 0)
            return Node[i].fullDepth * UCF(LENGTH);
        else
            return MISSING;

    case r_HEAD:
        if (i < 0)
            return MISSING;
        return (Node[i].newDepth + Node[i].invertElev) * UCF(LENGTH);

    case r_VOLUME:
        if (i < 0)
            return MISSING;
        return (Node[i].newVolume * UCF(VOLUME));

    case r_INFLOW:
        if (i < 0)
            return MISSING;
        else
            return Node[i].newLatFlow * UCF(FLOW);

    case r_TIMEOPEN:
        if (j < 0)
            return MISSING;
        if (Link[j].setting <= 0.0)
            return MISSING;
        return CurrentDate + CurrentTime - Link[j].timeLastSet;

    case r_TIMECLOSED:
        if (j < 0)
            return MISSING;
        if (Link[j].setting > 0.0)
            return MISSING;
        return CurrentDate + CurrentTime - Link[j].timeLastSet;

    default:
        return MISSING;
    }
}

/*!
* \brief Retrieves the current or past rainfall amount for a rain gage.
* \param[in] v Rule premise variable for a rain gage
* \return Current or past rainfall amount
*/
double getRainValue(struct TVariable v)
{
    if (v.index < 0)
        return MISSING;
    else if (Gage[v.index].isUsed == FALSE)
        return 0.0;
    else if (v.attribute == 0)
        return Gage[v.index].rainfall;
    else
        return gage_getPastRain(v.index, v.attribute);
}

/*!
* \brief Evaluates the truth of a relation between two date/times.
* \param[in] lhsValue Date/time value on left hand side of relation
* \param[in] relation Relational operator code (see RuleRelation enumeration)
* \param[in] rhsValue Date/time value on right hand side of relation
* \param[in] halfStep 1/2 the current time step (days)
* \return TRUE if time relation is satisfied, FALSE otherwise
*/
int compareTimes(double lhsValue, int relation, double rhsValue, double halfStep)
{
    if (relation == EQ)
    {
        if (lhsValue >= rhsValue - halfStep && lhsValue < rhsValue + halfStep)
            return TRUE;
        return FALSE;
    }
    else if (relation == NE)
    {
        if (lhsValue < rhsValue - halfStep || lhsValue >= rhsValue + halfStep)
            return TRUE;
        return FALSE;
    }
    else
        return compareValues(lhsValue, relation, rhsValue);
}

/*!
* \brief Evaluates the truth of a relation between two values.
* \param[in] lhsValue Value on left hand side of relation
* \param[in] relation Relational operator code (see RuleRelation enumeration)
* \param[in] rhsValue Value on right hand side of relation
* \return TRUE if relation is satisfied, FALSE otherwise
*/
int compareValues(double lhsValue, int relation, double rhsValue)
{
    SetPoint = rhsValue;
    ControlValue = lhsValue;
    switch (relation)
    {
    case EQ:
        if (lhsValue == rhsValue)
            return TRUE;
        break;
    case NE:
        if (lhsValue != rhsValue)
            return TRUE;
        break;
    case LT:
        if (lhsValue < rhsValue)
            return TRUE;
        break;
    case LE:
        if (lhsValue <= rhsValue)
            return TRUE;
        break;
    case GT:
        if (lhsValue > rhsValue)
            return TRUE;
        break;
    case GE:
        if (lhsValue >= rhsValue)
            return TRUE;
        break;
    }
    return FALSE;
}

/*!
* \brief Clears the list of actions to be executed.
*/
void clearActionList(void)
{
    struct TActionList *listItem;
    listItem = ActionList;
    while (listItem)
    {
        listItem->action = NULL;
        listItem = listItem->next;
    }
}

/*!
* \brief Frees the memory used to hold the list of actions to be executed.
*/
void deleteActionList(void)
{
    struct TActionList *listItem;
    struct TActionList *nextItem;
    listItem = ActionList;
    while (listItem)
    {
        nextItem = listItem->next;
        free(listItem);
        listItem = nextItem;
    }
    ActionList = NULL;
}

/*!
* \brief Frees the memory used for all of the control rules.
*/
void deleteRules(void)
{
    struct TPremise *p;
    struct TPremise *pnext;
    struct TAction *a;
    struct TAction *anext;
    int r;
    for (r = 0; r < RuleCount; r++)
    {
        p = Rules[r].firstPremise;
        while (p)
        {
            pnext = p->next;
            free(p);
            p = pnext;
        }
        a = Rules[r].thenActions;
        while (a)
        {
            anext = a->next;
            free(a);
            a = anext;
        }
        a = Rules[r].elseActions;
        while (a)
        {
            anext = a->next;
            free(a);
            a = anext;
        }
    }
    FREE(Rules);
    RuleCount = 0;
}

/*!
* \brief Finds the exact match between a string and an array of keyword strings.
* \param[in] s Character string
* \param[in] keyword Array of keyword strings
* \return Index of keyword which matches s or -1 if no match found
*/
int findExactMatch(char *s, char *keyword[])
{
    int i = 0;
    while (keyword[i] != NULL)
    {
        if (strcomp(s, keyword[i]))
            return (i);
        i++;
    }
    return (-1);
}
