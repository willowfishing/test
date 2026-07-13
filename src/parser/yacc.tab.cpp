/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output, and Bison version.  */
#define YYBISON 30802

/* Bison version string.  */
#define YYBISON_VERSION "3.8.2"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 2

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* First part of user prologue.  */
#line 1 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"

#include "ast.h"
#include "yacc.tab.h"
#include <iostream>
#include <memory>

int yylex(YYSTYPE *yylval, YYLTYPE *yylloc);

void yyerror(YYLTYPE *locp, const char* s) {
    std::cerr << "Parser Error at line " << locp->first_line << " column " << locp->first_column << ": " << s << std::endl;
}

using namespace ast;

// 全局变量：收集 JOIN ON 条件
static std::vector<std::shared_ptr<BinaryExpr>> g_on_conds;
// 全局变量：收集 UNION 子查询 (共享 ast.cpp 中的 g_union_map)
// 通过 ast::get_union_map() 访问

#line 91 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

#include "yacc.tab.h"
/* Symbol kind.  */
enum yysymbol_kind_t
{
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,                      /* "end of file"  */
  YYSYMBOL_YYerror = 1,                    /* error  */
  YYSYMBOL_YYUNDEF = 2,                    /* "invalid token"  */
  YYSYMBOL_SHOW = 3,                       /* SHOW  */
  YYSYMBOL_TABLES = 4,                     /* TABLES  */
  YYSYMBOL_CREATE = 5,                     /* CREATE  */
  YYSYMBOL_TABLE = 6,                      /* TABLE  */
  YYSYMBOL_DROP = 7,                       /* DROP  */
  YYSYMBOL_DESC = 8,                       /* DESC  */
  YYSYMBOL_INSERT = 9,                     /* INSERT  */
  YYSYMBOL_INTO = 10,                      /* INTO  */
  YYSYMBOL_VALUES = 11,                    /* VALUES  */
  YYSYMBOL_DELETE = 12,                    /* DELETE  */
  YYSYMBOL_FROM = 13,                      /* FROM  */
  YYSYMBOL_ASC = 14,                       /* ASC  */
  YYSYMBOL_ORDER = 15,                     /* ORDER  */
  YYSYMBOL_BY = 16,                        /* BY  */
  YYSYMBOL_WHERE = 17,                     /* WHERE  */
  YYSYMBOL_UPDATE = 18,                    /* UPDATE  */
  YYSYMBOL_SET = 19,                       /* SET  */
  YYSYMBOL_SELECT = 20,                    /* SELECT  */
  YYSYMBOL_INT = 21,                       /* INT  */
  YYSYMBOL_CHAR = 22,                      /* CHAR  */
  YYSYMBOL_FLOAT = 23,                     /* FLOAT  */
  YYSYMBOL_INDEX = 24,                     /* INDEX  */
  YYSYMBOL_AND = 25,                       /* AND  */
  YYSYMBOL_JOIN = 26,                      /* JOIN  */
  YYSYMBOL_ON = 27,                        /* ON  */
  YYSYMBOL_EXIT = 28,                      /* EXIT  */
  YYSYMBOL_HELP = 29,                      /* HELP  */
  YYSYMBOL_TXN_BEGIN = 30,                 /* TXN_BEGIN  */
  YYSYMBOL_TXN_COMMIT = 31,                /* TXN_COMMIT  */
  YYSYMBOL_TXN_ABORT = 32,                 /* TXN_ABORT  */
  YYSYMBOL_TXN_ROLLBACK = 33,              /* TXN_ROLLBACK  */
  YYSYMBOL_ORDER_BY = 34,                  /* ORDER_BY  */
  YYSYMBOL_ENABLE_NESTLOOP = 35,           /* ENABLE_NESTLOOP  */
  YYSYMBOL_ENABLE_SORTMERGE = 36,          /* ENABLE_SORTMERGE  */
  YYSYMBOL_EXPLAIN = 37,                   /* EXPLAIN  */
  YYSYMBOL_ANALYZE = 38,                   /* ANALYZE  */
  YYSYMBOL_COUNT = 39,                     /* COUNT  */
  YYSYMBOL_MAX = 40,                       /* MAX  */
  YYSYMBOL_MIN = 41,                       /* MIN  */
  YYSYMBOL_SUM = 42,                       /* SUM  */
  YYSYMBOL_AVG = 43,                       /* AVG  */
  YYSYMBOL_GROUP = 44,                     /* GROUP  */
  YYSYMBOL_HAVING = 45,                    /* HAVING  */
  YYSYMBOL_LIMIT = 46,                     /* LIMIT  */
  YYSYMBOL_AS = 47,                        /* AS  */
  YYSYMBOL_UNION = 48,                     /* UNION  */
  YYSYMBOL_TRANSACTION = 49,               /* TRANSACTION  */
  YYSYMBOL_ISOLATION = 50,                 /* ISOLATION  */
  YYSYMBOL_SNAPSHOT = 51,                  /* SNAPSHOT  */
  YYSYMBOL_LEVEL = 52,                     /* LEVEL  */
  YYSYMBOL_SERIALIZABLE = 53,              /* SERIALIZABLE  */
  YYSYMBOL_LEQ = 54,                       /* LEQ  */
  YYSYMBOL_NEQ = 55,                       /* NEQ  */
  YYSYMBOL_GEQ = 56,                       /* GEQ  */
  YYSYMBOL_T_EOF = 57,                     /* T_EOF  */
  YYSYMBOL_IDENTIFIER = 58,                /* IDENTIFIER  */
  YYSYMBOL_VALUE_STRING = 59,              /* VALUE_STRING  */
  YYSYMBOL_VALUE_INT = 60,                 /* VALUE_INT  */
  YYSYMBOL_VALUE_FLOAT = 61,               /* VALUE_FLOAT  */
  YYSYMBOL_VALUE_BOOL = 62,                /* VALUE_BOOL  */
  YYSYMBOL_63_ = 63,                       /* ';'  */
  YYSYMBOL_64_ = 64,                       /* '='  */
  YYSYMBOL_65_ = 65,                       /* '('  */
  YYSYMBOL_66_ = 66,                       /* ')'  */
  YYSYMBOL_67_ = 67,                       /* ','  */
  YYSYMBOL_68_ = 68,                       /* '.'  */
  YYSYMBOL_69_ = 69,                       /* '*'  */
  YYSYMBOL_70_ = 70,                       /* '<'  */
  YYSYMBOL_71_ = 71,                       /* '>'  */
  YYSYMBOL_72_ = 72,                       /* '+'  */
  YYSYMBOL_73_ = 73,                       /* '-'  */
  YYSYMBOL_YYACCEPT = 74,                  /* $accept  */
  YYSYMBOL_start = 75,                     /* start  */
  YYSYMBOL_stmt = 76,                      /* stmt  */
  YYSYMBOL_txnStmt = 77,                   /* txnStmt  */
  YYSYMBOL_dbStmt = 78,                    /* dbStmt  */
  YYSYMBOL_setStmt = 79,                   /* setStmt  */
  YYSYMBOL_ddl = 80,                       /* ddl  */
  YYSYMBOL_dml = 81,                       /* dml  */
  YYSYMBOL_selectWithin = 82,              /* selectWithin  */
  YYSYMBOL_unionQuery = 83,                /* unionQuery  */
  YYSYMBOL_fieldList = 84,                 /* fieldList  */
  YYSYMBOL_colNameList = 85,               /* colNameList  */
  YYSYMBOL_field = 86,                     /* field  */
  YYSYMBOL_type = 87,                      /* type  */
  YYSYMBOL_valueList = 88,                 /* valueList  */
  YYSYMBOL_value = 89,                     /* value  */
  YYSYMBOL_condition = 90,                 /* condition  */
  YYSYMBOL_optWhereClause = 91,            /* optWhereClause  */
  YYSYMBOL_whereClause = 92,               /* whereClause  */
  YYSYMBOL_col = 93,                       /* col  */
  YYSYMBOL_colList = 94,                   /* colList  */
  YYSYMBOL_op = 95,                        /* op  */
  YYSYMBOL_expr = 96,                      /* expr  */
  YYSYMBOL_setClauses = 97,                /* setClauses  */
  YYSYMBOL_setClause = 98,                 /* setClause  */
  YYSYMBOL_selector = 99,                  /* selector  */
  YYSYMBOL_opt_group_by = 100,             /* opt_group_by  */
  YYSYMBOL_opt_having = 101,               /* opt_having  */
  YYSYMBOL_opt_limit = 102,                /* opt_limit  */
  YYSYMBOL_tableRefs = 103,                /* tableRefs  */
  YYSYMBOL_tableRef = 104,                 /* tableRef  */
  YYSYMBOL_opt_order_clause = 105,         /* opt_order_clause  */
  YYSYMBOL_order_clause = 106,             /* order_clause  */
  YYSYMBOL_opt_asc_desc = 107,             /* opt_asc_desc  */
  YYSYMBOL_set_knob_type = 108,            /* set_knob_type  */
  YYSYMBOL_tbName = 109,                   /* tbName  */
  YYSYMBOL_colName = 110                   /* colName  */
};
typedef enum yysymbol_kind_t yysymbol_kind_t;




#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

/* Work around bug in HP-UX 11.23, which defines these macros
   incorrectly for preprocessor constants.  This workaround can likely
   be removed in 2023, as HPE has promised support for HP-UX 11.23
   (aka HP-UX 11i v2) only through the end of 2022; see Table 2 of
   <https://h20195.www2.hpe.com/V2/getpdf.aspx/4AA4-7673ENW.pdf>.  */
#ifdef __hpux
# undef UINT_LEAST8_MAX
# undef UINT_LEAST16_MAX
# define UINT_LEAST8_MAX 255
# define UINT_LEAST16_MAX 65535
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))


/* Stored state numbers (used for stacks). */
typedef yytype_uint8 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif


#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YY_USE(E) ((void) (E))
#else
# define YY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#if defined __GNUC__ && ! defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
# if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")
# else
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# endif
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if 1

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* 1 */

#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL \
             && defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
  YYLTYPE yyls_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE) \
             + YYSIZEOF (YYLTYPE)) \
      + 2 * YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  53
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   260

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  74
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  37
/* YYNRULES -- Number of rules.  */
#define YYNRULES  110
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  235

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   317


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK                     \
   ? YY_CAST (yysymbol_kind_t, yytranslate[YYX])        \
   : YYSYMBOL_YYUNDEF)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
      65,    66,    69,    72,    67,    73,    68,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,    63,
      70,    64,    71,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    58,    59,    60,    61,    62
};

#if YYDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_int16 yyrline[] =
{
       0,    75,    75,    80,    85,    90,    98,    99,   100,   101,
     102,   106,   110,   114,   118,   125,   129,   138,   142,   146,
     153,   157,   161,   165,   169,   176,   180,   184,   188,   208,
     233,   256,   263,   272,   276,   283,   287,   294,   301,   305,
     309,   316,   320,   327,   331,   335,   339,   346,   353,   354,
     361,   365,   372,   376,   380,   384,   388,   393,   398,   403,
     408,   413,   418,   423,   428,   433,   438,   443,   451,   455,
     462,   466,   470,   474,   478,   482,   489,   493,   500,   504,
     511,   515,   522,   538,   542,   549,   550,   557,   558,   565,
     566,   574,   578,   582,   590,   599,   603,   608,   613,   618,
     626,   630,   634,   638,   646,   647,   648,   652,   653,   656,
     658
};
#endif

/** Accessing symbol of state STATE.  */
#define YY_ACCESSING_SYMBOL(State) YY_CAST (yysymbol_kind_t, yystos[State])

#if 1
/* The user-facing name of the symbol whose (internal) number is
   YYSYMBOL.  No bounds checking.  */
static const char *yysymbol_name (yysymbol_kind_t yysymbol) YY_ATTRIBUTE_UNUSED;

/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "\"end of file\"", "error", "\"invalid token\"", "SHOW", "TABLES",
  "CREATE", "TABLE", "DROP", "DESC", "INSERT", "INTO", "VALUES", "DELETE",
  "FROM", "ASC", "ORDER", "BY", "WHERE", "UPDATE", "SET", "SELECT", "INT",
  "CHAR", "FLOAT", "INDEX", "AND", "JOIN", "ON", "EXIT", "HELP",
  "TXN_BEGIN", "TXN_COMMIT", "TXN_ABORT", "TXN_ROLLBACK", "ORDER_BY",
  "ENABLE_NESTLOOP", "ENABLE_SORTMERGE", "EXPLAIN", "ANALYZE", "COUNT",
  "MAX", "MIN", "SUM", "AVG", "GROUP", "HAVING", "LIMIT", "AS", "UNION",
  "TRANSACTION", "ISOLATION", "SNAPSHOT", "LEVEL", "SERIALIZABLE", "LEQ",
  "NEQ", "GEQ", "T_EOF", "IDENTIFIER", "VALUE_STRING", "VALUE_INT",
  "VALUE_FLOAT", "VALUE_BOOL", "';'", "'='", "'('", "')'", "','", "'.'",
  "'*'", "'<'", "'>'", "'+'", "'-'", "$accept", "start", "stmt", "txnStmt",
  "dbStmt", "setStmt", "ddl", "dml", "selectWithin", "unionQuery",
  "fieldList", "colNameList", "field", "type", "valueList", "value",
  "condition", "optWhereClause", "whereClause", "col", "colList", "op",
  "expr", "setClauses", "setClause", "selector", "opt_group_by",
  "opt_having", "opt_limit", "tableRefs", "tableRef", "opt_order_clause",
  "order_clause", "opt_asc_desc", "set_knob_type", "tbName", "colName", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-145)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-110)

#define yytable_value_is_error(Yyn) \
  0

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int16 yypact[] =
{
      73,    18,    21,    29,   -39,    31,    39,   -39,    22,    -3,
    -145,  -145,  -145,  -145,  -145,  -145,     6,  -145,    56,    26,
    -145,  -145,  -145,  -145,  -145,  -145,   114,   -39,   -39,   -39,
     -39,  -145,  -145,   -39,   -39,   112,  -145,  -145,    88,    76,
      77,    81,   109,   110,   111,   113,  -145,  -145,   115,   164,
     116,   131,   159,  -145,  -145,   -39,   118,   120,  -145,   121,
     169,   170,   130,   137,   129,    74,     7,     7,     7,     7,
       7,    19,   130,   134,    -3,  -145,   130,   130,   130,   132,
       7,  -145,  -145,    -6,  -145,   135,   -37,  -145,   128,   136,
     138,   139,   140,   141,  -145,   175,    -5,  -145,    25,   149,
    -145,   185,    33,  -145,    86,    45,  -145,    78,    59,  -145,
     176,    93,   130,  -145,   108,   150,  -145,   156,   161,   162,
     163,   165,   166,    -3,   167,   -35,    19,    19,   172,   -39,
    -145,   153,    19,  -145,   130,  -145,   152,  -145,  -145,  -145,
     130,  -145,  -145,  -145,  -145,  -145,    84,  -145,     7,  -145,
    -145,  -145,  -145,  -145,  -145,    94,  -145,  -145,    87,  -145,
     160,   168,   171,   173,   174,   177,   201,   175,   175,    40,
     192,  -145,   204,   178,  -145,  -145,    -5,  -145,   179,  -145,
    -145,    59,  -145,  -145,  -145,  -145,    59,    59,  -145,  -145,
    -145,  -145,  -145,  -145,    19,  -145,  -145,   -39,  -145,     7,
       7,     7,   206,   172,   158,  -145,  -145,  -145,    -5,  -145,
     176,   115,   176,   209,   181,   178,  -145,   172,     7,   180,
    -145,   206,   178,    46,   155,  -145,   181,  -145,  -145,  -145,
    -145,     7,  -145,    46,  -145
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_int8 yydefact[] =
{
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       4,     3,    11,    12,    13,    14,     0,     5,     0,     0,
       9,     6,    10,     7,     8,    15,     0,     0,     0,     0,
       0,   109,    22,     0,     0,     0,   107,   108,     0,     0,
       0,     0,     0,     0,     0,   110,    83,    68,    84,     0,
       0,    54,     0,     1,     2,     0,     0,     0,    21,     0,
       0,    48,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,    16,     0,     0,     0,     0,
       0,    26,   110,    48,    78,     0,     0,    17,     0,     0,
       0,     0,     0,     0,    69,     0,    48,    91,    95,    52,
      55,     0,     0,    33,     0,     0,    35,     0,     0,    50,
      49,     0,     0,    27,     0,     0,    19,    56,    58,    60,
      62,    64,    66,     0,     0,     0,     0,     0,    85,     0,
      96,     0,     0,    20,     0,    38,     0,    40,    37,    23,
       0,    24,    45,    43,    44,    46,     0,    41,     0,    74,
      73,    75,    70,    71,    72,     0,    79,    80,     0,    18,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
      94,    92,     0,    87,    97,    53,    48,    34,     0,    36,
      25,     0,    51,    76,    77,    47,     0,     0,    57,    59,
      61,    63,    65,    67,     0,    31,    32,     0,    98,     0,
       0,     0,   101,    85,     0,    42,    81,    82,    48,    99,
      93,    86,    88,     0,    89,    87,    39,    85,     0,     0,
      28,   101,    87,   106,   100,    90,    89,    30,   105,   104,
     102,     0,    29,   106,   103
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -145,  -145,  -145,  -145,  -145,  -145,  -145,  -145,     5,  -145,
    -145,   182,    96,  -145,  -145,  -112,    80,   -79,   -75,   -60,
      34,  -145,  -145,  -145,   124,   -59,  -144,  -136,    11,  -131,
      35,    12,  -145,     8,  -145,    -4,   -44
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_uint8 yydefgoto[] =
{
       0,    18,    19,    20,    21,    22,    23,    24,   124,   125,
     102,   105,   103,   138,   146,   147,   109,    81,   110,    47,
      48,   155,   185,    83,    84,    49,   173,   202,   220,    96,
      97,   214,   224,   230,    39,    50,    51
};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int16 yytable[] =
{
      32,   176,   157,    35,   113,    89,    90,    91,    92,    93,
      94,    80,    80,   168,   115,   101,   116,   128,    85,    31,
     111,   126,    25,    56,    57,    58,    59,    27,    99,    60,
      61,   169,   104,   106,   106,    29,    40,    41,    42,    43,
      44,    33,    26,   183,    52,    28,    40,    41,    42,    43,
      44,    75,    34,    30,   228,    45,    53,    36,    37,   215,
     229,   112,   127,   208,   166,    45,    46,    98,    85,   205,
     158,    38,   129,   222,   206,   207,     1,    31,     2,   221,
       3,     4,     5,    31,    95,     6,   227,   197,   111,    54,
     104,     7,     8,     9,   130,   184,   179,   203,    31,   133,
     134,    10,    11,    12,    13,    14,    15,   135,   136,   137,
      16,   139,   140,    40,    41,    42,    43,    44,   142,   143,
     144,   145,    98,    98,   210,   174,   212,    55,    98,   217,
      17,    62,    45,    40,    41,    42,    43,    44,    63,   111,
      64,   111,    65,    88,   141,   140,    66,   149,   150,   151,
     180,   181,    45,   142,   143,   144,   145,   152,   223,   186,
     187,   170,   171,   153,   154,   198,    82,   142,   143,   144,
     145,   233,   195,   196,    67,    68,    69,    71,    73,    74,
      79,  -109,    70,    76,    72,    77,    78,    80,    82,    86,
      98,    87,   100,   209,   117,   123,   131,   108,   132,   114,
     159,   148,   118,   160,   119,   120,   121,   122,   161,   162,
     163,   175,   164,   165,   194,   167,   172,   178,   188,   199,
     200,   213,   231,   201,   216,   218,   189,   219,   182,   190,
     177,   191,   192,   226,   211,   193,   156,   232,     0,   204,
     225,   234,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     107
};

static const yytype_int16 yycheck[] =
{
       4,   132,   114,     7,    83,    65,    66,    67,    68,    69,
      70,    17,    17,    48,    51,    74,    53,    96,    62,    58,
      80,    26,     4,    27,    28,    29,    30,     6,    72,    33,
      34,    66,    76,    77,    78,     6,    39,    40,    41,    42,
      43,    10,    24,   155,    38,    24,    39,    40,    41,    42,
      43,    55,    13,    24,     8,    58,     0,    35,    36,   203,
      14,    67,    67,   194,   123,    58,    69,    71,   112,   181,
     114,    49,    47,   217,   186,   187,     3,    58,     5,   215,
       7,     8,     9,    58,    65,    12,   222,    47,   148,    63,
     134,    18,    19,    20,    98,   155,   140,   176,    58,    66,
      67,    28,    29,    30,    31,    32,    33,    21,    22,    23,
      37,    66,    67,    39,    40,    41,    42,    43,    59,    60,
      61,    62,   126,   127,   199,   129,   201,    13,   132,   208,
      57,    19,    58,    39,    40,    41,    42,    43,    50,   199,
      64,   201,    65,    69,    66,    67,    65,    54,    55,    56,
      66,    67,    58,    59,    60,    61,    62,    64,   218,    72,
      73,   126,   127,    70,    71,   169,    58,    59,    60,    61,
      62,   231,   167,   168,    65,    65,    65,    13,    47,    20,
      11,    68,    67,    65,    68,    65,    65,    17,    58,    52,
     194,    62,    58,   197,    66,    20,    47,    65,    13,    64,
      50,    25,    66,    47,    66,    66,    66,    66,    47,    47,
      47,    58,    47,    47,    13,    48,    44,    65,    58,    27,
      16,    15,    67,    45,    66,    16,    58,    46,   148,    58,
     134,    58,    58,   221,   200,    58,   112,   226,    -1,    60,
      60,   233,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      78
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_int8 yystos[] =
{
       0,     3,     5,     7,     8,     9,    12,    18,    19,    20,
      28,    29,    30,    31,    32,    33,    37,    57,    75,    76,
      77,    78,    79,    80,    81,     4,    24,     6,    24,     6,
      24,    58,   109,    10,    13,   109,    35,    36,    49,   108,
      39,    40,    41,    42,    43,    58,    69,    93,    94,    99,
     109,   110,    38,     0,    63,    13,   109,   109,   109,   109,
     109,   109,    19,    50,    64,    65,    65,    65,    65,    65,
      67,    13,    68,    47,    20,   109,    65,    65,    65,    11,
      17,    91,    58,    97,    98,   110,    52,    62,    69,    93,
      93,    93,    93,    93,    93,    65,   103,   104,   109,   110,
      58,    99,    84,    86,   110,    85,   110,    85,    65,    90,
      92,    93,    67,    91,    64,    51,    53,    66,    66,    66,
      66,    66,    66,    20,    82,    83,    26,    67,    91,    47,
     109,    47,    13,    66,    67,    21,    22,    23,    87,    66,
      67,    66,    59,    60,    61,    62,    88,    89,    25,    54,
      55,    56,    64,    70,    71,    95,    98,    89,   110,    50,
      47,    47,    47,    47,    47,    47,    99,    48,    48,    66,
     104,   104,    44,   100,   109,    58,   103,    86,    65,   110,
      66,    67,    90,    89,    93,    96,    72,    73,    58,    58,
      58,    58,    58,    58,    13,    82,    82,    47,   109,    27,
      16,    45,   101,    91,    60,    89,    89,    89,   103,   109,
      92,    94,    92,    15,   105,   100,    66,    91,    16,    46,
     102,   101,   100,    93,   106,    60,   105,   101,     8,    14,
     107,    67,   102,    93,   107
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr1[] =
{
       0,    74,    75,    75,    75,    75,    76,    76,    76,    76,
      76,    77,    77,    77,    77,    78,    78,    79,    79,    79,
      80,    80,    80,    80,    80,    81,    81,    81,    81,    81,
      82,    83,    83,    84,    84,    85,    85,    86,    87,    87,
      87,    88,    88,    89,    89,    89,    89,    90,    91,    91,
      92,    92,    93,    93,    93,    93,    93,    93,    93,    93,
      93,    93,    93,    93,    93,    93,    93,    93,    94,    94,
      95,    95,    95,    95,    95,    95,    96,    96,    97,    97,
      98,    98,    98,    99,    99,   100,   100,   101,   101,   102,
     102,   103,   103,   103,   103,   104,   104,   104,   104,   104,
     105,   105,   106,   106,   107,   107,   107,   108,   108,   109,
     110
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     2,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     2,     4,     4,     6,     5,
       6,     3,     2,     6,     6,     7,     4,     5,     9,    11,
       7,     3,     3,     1,     3,     1,     3,     2,     1,     4,
       1,     1,     3,     1,     1,     1,     1,     3,     0,     2,
       1,     3,     3,     5,     1,     3,     4,     6,     4,     6,
       4,     6,     4,     6,     4,     6,     4,     6,     1,     3,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     3,
       3,     5,     5,     1,     1,     0,     3,     0,     2,     0,
       2,     1,     3,     5,     3,     1,     2,     3,     4,     5,
       3,     0,     2,     4,     1,     1,     0,     1,     1,     1,
       1
};


enum { YYENOMEM = -2 };

#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYNOMEM         goto yyexhaustedlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
  do                                                              \
    if (yychar == YYEMPTY)                                        \
      {                                                           \
        yychar = (Token);                                         \
        yylval = (Value);                                         \
        YYPOPSTACK (yylen);                                       \
        yystate = *yyssp;                                         \
        goto yybackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        yyerror (&yylloc, YY_("syntax error: cannot back up")); \
        YYERROR;                                                  \
      }                                                           \
  while (0)

/* Backward compatibility with an undocumented macro.
   Use YYerror or YYUNDEF. */
#define YYERRCODE YYUNDEF

/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

#ifndef YYLLOC_DEFAULT
# define YYLLOC_DEFAULT(Current, Rhs, N)                                \
    do                                                                  \
      if (N)                                                            \
        {                                                               \
          (Current).first_line   = YYRHSLOC (Rhs, 1).first_line;        \
          (Current).first_column = YYRHSLOC (Rhs, 1).first_column;      \
          (Current).last_line    = YYRHSLOC (Rhs, N).last_line;         \
          (Current).last_column  = YYRHSLOC (Rhs, N).last_column;       \
        }                                                               \
      else                                                              \
        {                                                               \
          (Current).first_line   = (Current).last_line   =              \
            YYRHSLOC (Rhs, 0).last_line;                                \
          (Current).first_column = (Current).last_column =              \
            YYRHSLOC (Rhs, 0).last_column;                              \
        }                                                               \
    while (0)
#endif

#define YYRHSLOC(Rhs, K) ((Rhs)[K])


/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)


/* YYLOCATION_PRINT -- Print the location on the stream.
   This macro was not mandated originally: define only if we know
   we won't break user code: when these are the locations we know.  */

# ifndef YYLOCATION_PRINT

#  if defined YY_LOCATION_PRINT

   /* Temporary convenience wrapper in case some people defined the
      undocumented and private YY_LOCATION_PRINT macros.  */
#   define YYLOCATION_PRINT(File, Loc)  YY_LOCATION_PRINT(File, *(Loc))

#  elif defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL

/* Print *YYLOCP on YYO.  Private, do not rely on its existence. */

YY_ATTRIBUTE_UNUSED
static int
yy_location_print_ (FILE *yyo, YYLTYPE const * const yylocp)
{
  int res = 0;
  int end_col = 0 != yylocp->last_column ? yylocp->last_column - 1 : 0;
  if (0 <= yylocp->first_line)
    {
      res += YYFPRINTF (yyo, "%d", yylocp->first_line);
      if (0 <= yylocp->first_column)
        res += YYFPRINTF (yyo, ".%d", yylocp->first_column);
    }
  if (0 <= yylocp->last_line)
    {
      if (yylocp->first_line < yylocp->last_line)
        {
          res += YYFPRINTF (yyo, "-%d", yylocp->last_line);
          if (0 <= end_col)
            res += YYFPRINTF (yyo, ".%d", end_col);
        }
      else if (0 <= end_col && yylocp->first_column < end_col)
        res += YYFPRINTF (yyo, "-%d", end_col);
    }
  return res;
}

#   define YYLOCATION_PRINT  yy_location_print_

    /* Temporary convenience wrapper in case some people defined the
       undocumented and private YY_LOCATION_PRINT macros.  */
#   define YY_LOCATION_PRINT(File, Loc)  YYLOCATION_PRINT(File, &(Loc))

#  else

#   define YYLOCATION_PRINT(File, Loc) ((void) 0)
    /* Temporary convenience wrapper in case some people defined the
       undocumented and private YY_LOCATION_PRINT macros.  */
#   define YY_LOCATION_PRINT  YYLOCATION_PRINT

#  endif
# endif /* !defined YYLOCATION_PRINT */


# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Kind, Value, Location); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo,
                       yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep, YYLTYPE const * const yylocationp)
{
  FILE *yyoutput = yyo;
  YY_USE (yyoutput);
  YY_USE (yylocationp);
  if (!yyvaluep)
    return;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo,
                 yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep, YYLTYPE const * const yylocationp)
{
  YYFPRINTF (yyo, "%s %s (",
             yykind < YYNTOKENS ? "token" : "nterm", yysymbol_name (yykind));

  YYLOCATION_PRINT (yyo, yylocationp);
  YYFPRINTF (yyo, ": ");
  yy_symbol_value_print (yyo, yykind, yyvaluep, yylocationp);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp, YYLTYPE *yylsp,
                 int yyrule)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       YY_ACCESSING_SYMBOL (+yyssp[yyi + 1 - yynrhs]),
                       &yyvsp[(yyi + 1) - (yynrhs)],
                       &(yylsp[(yyi + 1) - (yynrhs)]));
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, yylsp, Rule); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args) ((void) 0)
# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif


/* Context of a parse error.  */
typedef struct
{
  yy_state_t *yyssp;
  yysymbol_kind_t yytoken;
  YYLTYPE *yylloc;
} yypcontext_t;

/* Put in YYARG at most YYARGN of the expected tokens given the
   current YYCTX, and return the number of tokens stored in YYARG.  If
   YYARG is null, return the number of expected tokens (guaranteed to
   be less than YYNTOKENS).  Return YYENOMEM on memory exhaustion.
   Return 0 if there are more than YYARGN expected tokens, yet fill
   YYARG up to YYARGN. */
static int
yypcontext_expected_tokens (const yypcontext_t *yyctx,
                            yysymbol_kind_t yyarg[], int yyargn)
{
  /* Actual size of YYARG. */
  int yycount = 0;
  int yyn = yypact[+*yyctx->yyssp];
  if (!yypact_value_is_default (yyn))
    {
      /* Start YYX at -YYN if negative to avoid negative indexes in
         YYCHECK.  In other words, skip the first -YYN actions for
         this state because they are default actions.  */
      int yyxbegin = yyn < 0 ? -yyn : 0;
      /* Stay within bounds of both yycheck and yytname.  */
      int yychecklim = YYLAST - yyn + 1;
      int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
      int yyx;
      for (yyx = yyxbegin; yyx < yyxend; ++yyx)
        if (yycheck[yyx + yyn] == yyx && yyx != YYSYMBOL_YYerror
            && !yytable_value_is_error (yytable[yyx + yyn]))
          {
            if (!yyarg)
              ++yycount;
            else if (yycount == yyargn)
              return 0;
            else
              yyarg[yycount++] = YY_CAST (yysymbol_kind_t, yyx);
          }
    }
  if (yyarg && yycount == 0 && 0 < yyargn)
    yyarg[0] = YYSYMBOL_YYEMPTY;
  return yycount;
}




#ifndef yystrlen
# if defined __GLIBC__ && defined _STRING_H
#  define yystrlen(S) (YY_CAST (YYPTRDIFF_T, strlen (S)))
# else
/* Return the length of YYSTR.  */
static YYPTRDIFF_T
yystrlen (const char *yystr)
{
  YYPTRDIFF_T yylen;
  for (yylen = 0; yystr[yylen]; yylen++)
    continue;
  return yylen;
}
# endif
#endif

#ifndef yystpcpy
# if defined __GLIBC__ && defined _STRING_H && defined _GNU_SOURCE
#  define yystpcpy stpcpy
# else
/* Copy YYSRC to YYDEST, returning the address of the terminating '\0' in
   YYDEST.  */
static char *
yystpcpy (char *yydest, const char *yysrc)
{
  char *yyd = yydest;
  const char *yys = yysrc;

  while ((*yyd++ = *yys++) != '\0')
    continue;

  return yyd - 1;
}
# endif
#endif

#ifndef yytnamerr
/* Copy to YYRES the contents of YYSTR after stripping away unnecessary
   quotes and backslashes, so that it's suitable for yyerror.  The
   heuristic is that double-quoting is unnecessary unless the string
   contains an apostrophe, a comma, or backslash (other than
   backslash-backslash).  YYSTR is taken from yytname.  If YYRES is
   null, do not copy; instead, return the length of what the result
   would have been.  */
static YYPTRDIFF_T
yytnamerr (char *yyres, const char *yystr)
{
  if (*yystr == '"')
    {
      YYPTRDIFF_T yyn = 0;
      char const *yyp = yystr;
      for (;;)
        switch (*++yyp)
          {
          case '\'':
          case ',':
            goto do_not_strip_quotes;

          case '\\':
            if (*++yyp != '\\')
              goto do_not_strip_quotes;
            else
              goto append;

          append:
          default:
            if (yyres)
              yyres[yyn] = *yyp;
            yyn++;
            break;

          case '"':
            if (yyres)
              yyres[yyn] = '\0';
            return yyn;
          }
    do_not_strip_quotes: ;
    }

  if (yyres)
    return yystpcpy (yyres, yystr) - yyres;
  else
    return yystrlen (yystr);
}
#endif


static int
yy_syntax_error_arguments (const yypcontext_t *yyctx,
                           yysymbol_kind_t yyarg[], int yyargn)
{
  /* Actual size of YYARG. */
  int yycount = 0;
  /* There are many possibilities here to consider:
     - If this state is a consistent state with a default action, then
       the only way this function was invoked is if the default action
       is an error action.  In that case, don't check for expected
       tokens because there are none.
     - The only way there can be no lookahead present (in yychar) is if
       this state is a consistent state with a default action.  Thus,
       detecting the absence of a lookahead is sufficient to determine
       that there is no unexpected or expected token to report.  In that
       case, just report a simple "syntax error".
     - Don't assume there isn't a lookahead just because this state is a
       consistent state with a default action.  There might have been a
       previous inconsistent state, consistent state with a non-default
       action, or user semantic action that manipulated yychar.
     - Of course, the expected token list depends on states to have
       correct lookahead information, and it depends on the parser not
       to perform extra reductions after fetching a lookahead from the
       scanner and before detecting a syntax error.  Thus, state merging
       (from LALR or IELR) and default reductions corrupt the expected
       token list.  However, the list is correct for canonical LR with
       one exception: it will still contain any token that will not be
       accepted due to an error action in a later state.
  */
  if (yyctx->yytoken != YYSYMBOL_YYEMPTY)
    {
      int yyn;
      if (yyarg)
        yyarg[yycount] = yyctx->yytoken;
      ++yycount;
      yyn = yypcontext_expected_tokens (yyctx,
                                        yyarg ? yyarg + 1 : yyarg, yyargn - 1);
      if (yyn == YYENOMEM)
        return YYENOMEM;
      else
        yycount += yyn;
    }
  return yycount;
}

/* Copy into *YYMSG, which is of size *YYMSG_ALLOC, an error message
   about the unexpected token YYTOKEN for the state stack whose top is
   YYSSP.

   Return 0 if *YYMSG was successfully written.  Return -1 if *YYMSG is
   not large enough to hold the message.  In that case, also set
   *YYMSG_ALLOC to the required number of bytes.  Return YYENOMEM if the
   required number of bytes is too large to store.  */
static int
yysyntax_error (YYPTRDIFF_T *yymsg_alloc, char **yymsg,
                const yypcontext_t *yyctx)
{
  enum { YYARGS_MAX = 5 };
  /* Internationalized format string. */
  const char *yyformat = YY_NULLPTR;
  /* Arguments of yyformat: reported tokens (one for the "unexpected",
     one per "expected"). */
  yysymbol_kind_t yyarg[YYARGS_MAX];
  /* Cumulated lengths of YYARG.  */
  YYPTRDIFF_T yysize = 0;

  /* Actual size of YYARG. */
  int yycount = yy_syntax_error_arguments (yyctx, yyarg, YYARGS_MAX);
  if (yycount == YYENOMEM)
    return YYENOMEM;

  switch (yycount)
    {
#define YYCASE_(N, S)                       \
      case N:                               \
        yyformat = S;                       \
        break
    default: /* Avoid compiler warnings. */
      YYCASE_(0, YY_("syntax error"));
      YYCASE_(1, YY_("syntax error, unexpected %s"));
      YYCASE_(2, YY_("syntax error, unexpected %s, expecting %s"));
      YYCASE_(3, YY_("syntax error, unexpected %s, expecting %s or %s"));
      YYCASE_(4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
      YYCASE_(5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
#undef YYCASE_
    }

  /* Compute error message size.  Don't count the "%s"s, but reserve
     room for the terminator.  */
  yysize = yystrlen (yyformat) - 2 * yycount + 1;
  {
    int yyi;
    for (yyi = 0; yyi < yycount; ++yyi)
      {
        YYPTRDIFF_T yysize1
          = yysize + yytnamerr (YY_NULLPTR, yytname[yyarg[yyi]]);
        if (yysize <= yysize1 && yysize1 <= YYSTACK_ALLOC_MAXIMUM)
          yysize = yysize1;
        else
          return YYENOMEM;
      }
  }

  if (*yymsg_alloc < yysize)
    {
      *yymsg_alloc = 2 * yysize;
      if (! (yysize <= *yymsg_alloc
             && *yymsg_alloc <= YYSTACK_ALLOC_MAXIMUM))
        *yymsg_alloc = YYSTACK_ALLOC_MAXIMUM;
      return -1;
    }

  /* Avoid sprintf, as that infringes on the user's name space.
     Don't have undefined behavior even if the translation
     produced a string with the wrong number of "%s"s.  */
  {
    char *yyp = *yymsg;
    int yyi = 0;
    while ((*yyp = *yyformat) != '\0')
      if (*yyp == '%' && yyformat[1] == 's' && yyi < yycount)
        {
          yyp += yytnamerr (yyp, yytname[yyarg[yyi++]]);
          yyformat += 2;
        }
      else
        {
          ++yyp;
          ++yyformat;
        }
  }
  return 0;
}


/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg,
            yysymbol_kind_t yykind, YYSTYPE *yyvaluep, YYLTYPE *yylocationp)
{
  YY_USE (yyvaluep);
  YY_USE (yylocationp);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}






/*----------.
| yyparse.  |
`----------*/

int
yyparse (void)
{
/* Lookahead token kind.  */
int yychar;


/* The semantic value of the lookahead symbol.  */
/* Default value used for initialization, for pacifying older GCCs
   or non-GCC compilers.  */
YY_INITIAL_VALUE (static YYSTYPE yyval_default;)
YYSTYPE yylval YY_INITIAL_VALUE (= yyval_default);

/* Location data for the lookahead symbol.  */
static YYLTYPE yyloc_default
# if defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL
  = { 1, 1, 1, 1 }
# endif
;
YYLTYPE yylloc = yyloc_default;

    /* Number of syntax errors so far.  */
    int yynerrs = 0;

    yy_state_fast_t yystate = 0;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus = 0;

    /* Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* Their size.  */
    YYPTRDIFF_T yystacksize = YYINITDEPTH;

    /* The state stack: array, bottom, top.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss = yyssa;
    yy_state_t *yyssp = yyss;

    /* The semantic value stack: array, bottom, top.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs = yyvsa;
    YYSTYPE *yyvsp = yyvs;

    /* The location stack: array, bottom, top.  */
    YYLTYPE yylsa[YYINITDEPTH];
    YYLTYPE *yyls = yylsa;
    YYLTYPE *yylsp = yyls;

  int yyn;
  /* The return value of yyparse.  */
  int yyresult;
  /* Lookahead symbol kind.  */
  yysymbol_kind_t yytoken = YYSYMBOL_YYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;
  YYLTYPE yyloc;

  /* The locations where the error started and ended.  */
  YYLTYPE yyerror_range[3];

  /* Buffer for error messages, and its allocated size.  */
  char yymsgbuf[128];
  char *yymsg = yymsgbuf;
  YYPTRDIFF_T yymsg_alloc = sizeof yymsgbuf;

#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N), yylsp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yychar = YYEMPTY; /* Cause a token to be read.  */

  yylsp[0] = yylloc;
  goto yysetstate;


/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END
  YY_STACK_PRINT (yyss, yyssp);

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    YYNOMEM;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;
        YYLTYPE *yyls1 = yyls;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
                    &yyls1, yysize * YYSIZEOF (*yylsp),
                    &yystacksize);
        yyss = yyss1;
        yyvs = yyvs1;
        yyls = yyls1;
      }
# else /* defined YYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        YYNOMEM;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          YYNOMEM;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
        YYSTACK_RELOCATE (yyls_alloc, yyls);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;
      yylsp = yyls + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */


  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;


/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token\n"));
      yychar = yylex (&yylval, &yylloc);
    }

  if (yychar <= YYEOF)
    {
      yychar = YYEOF;
      yytoken = YYSYMBOL_YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else if (yychar == YYerror)
    {
      /* The scanner already issued an error message, process directly
         to error recovery.  But do not keep the error token as
         lookahead, it is too special and may lead us to an endless
         loop in error recovery. */
      yychar = YYUNDEF;
      yytoken = YYSYMBOL_YYerror;
      yyerror_range[1] = yylloc;
      goto yyerrlab1;
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END
  *++yylsp = yylloc;

  /* Discard the shifted token.  */
  yychar = YYEMPTY;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];

  /* Default location. */
  YYLLOC_DEFAULT (yyloc, (yylsp - yylen), yylen);
  yyerror_range[1] = yyloc;
  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
  case 2: /* start: stmt ';'  */
#line 76 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        parse_tree = (yyvsp[-1].sv_node);
        YYACCEPT;
    }
#line 1753 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 3: /* start: HELP  */
#line 81 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        parse_tree = std::make_shared<Help>();
        YYACCEPT;
    }
#line 1762 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 4: /* start: EXIT  */
#line 86 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        parse_tree = nullptr;
        YYACCEPT;
    }
#line 1771 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 5: /* start: T_EOF  */
#line 91 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        parse_tree = nullptr;
        YYACCEPT;
    }
#line 1780 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 11: /* txnStmt: TXN_BEGIN  */
#line 107 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<TxnBegin>();
    }
#line 1788 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 12: /* txnStmt: TXN_COMMIT  */
#line 111 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<TxnCommit>();
    }
#line 1796 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 13: /* txnStmt: TXN_ABORT  */
#line 115 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<TxnAbort>();
    }
#line 1804 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 14: /* txnStmt: TXN_ROLLBACK  */
#line 119 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<TxnRollback>();
    }
#line 1812 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 15: /* dbStmt: SHOW TABLES  */
#line 126 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<ShowTables>();
    }
#line 1820 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 16: /* dbStmt: SHOW INDEX FROM tbName  */
#line 130 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto *n = new ShowIndex();
        n->tab_name = (yyvsp[0].sv_str);
        (yyval.sv_node) = std::shared_ptr<ShowIndex>(n);
    }
#line 1830 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 17: /* setStmt: SET set_knob_type '=' VALUE_BOOL  */
#line 139 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<SetStmt>((yyvsp[-2].sv_setKnobType), (yyvsp[0].sv_bool));
    }
#line 1838 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 18: /* setStmt: SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION  */
#line 143 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<SetIsolationLevel>(IsoLevelSyntax::SNAPSHOT_ISOLATION);
    }
#line 1846 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 19: /* setStmt: SET TRANSACTION ISOLATION LEVEL SERIALIZABLE  */
#line 147 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<SetIsolationLevel>(IsoLevelSyntax::SERIALIZABLE);
    }
#line 1854 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 20: /* ddl: CREATE TABLE tbName '(' fieldList ')'  */
#line 154 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<CreateTable>((yyvsp[-3].sv_str), (yyvsp[-1].sv_fields));
    }
#line 1862 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 21: /* ddl: DROP TABLE tbName  */
#line 158 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<DropTable>((yyvsp[0].sv_str));
    }
#line 1870 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 22: /* ddl: DESC tbName  */
#line 162 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<DescTable>((yyvsp[0].sv_str));
    }
#line 1878 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 23: /* ddl: CREATE INDEX tbName '(' colNameList ')'  */
#line 166 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<CreateIndex>((yyvsp[-3].sv_str), (yyvsp[-1].sv_strs));
    }
#line 1886 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 24: /* ddl: DROP INDEX tbName '(' colNameList ')'  */
#line 170 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<DropIndex>((yyvsp[-3].sv_str), (yyvsp[-1].sv_strs));
    }
#line 1894 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 25: /* dml: INSERT INTO tbName VALUES '(' valueList ')'  */
#line 177 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<InsertStmt>((yyvsp[-4].sv_str), (yyvsp[-1].sv_vals));
    }
#line 1902 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 26: /* dml: DELETE FROM tbName optWhereClause  */
#line 181 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<DeleteStmt>((yyvsp[-1].sv_str), (yyvsp[0].sv_conds));
    }
#line 1910 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 27: /* dml: UPDATE tbName SET setClauses optWhereClause  */
#line 185 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_node) = std::make_shared<UpdateStmt>((yyvsp[-3].sv_str), (yyvsp[-1].sv_set_clauses), (yyvsp[0].sv_conds));
    }
#line 1918 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 28: /* dml: SELECT selector FROM tableRefs optWhereClause opt_group_by opt_having opt_order_clause opt_limit  */
#line 189 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto conds = (yyvsp[-4].sv_conds);
        for (auto &cond : g_on_conds) {
            conds.push_back(cond);
        }
        g_on_conds.clear();
        std::vector<std::string> tables;
        std::vector<std::string> aliases;
        for (size_t i = 0; i < (yyvsp[-5].sv_strs).size(); i += 2) {
            tables.push_back((yyvsp[-5].sv_strs)[i]);
            if (i + 1 < (yyvsp[-5].sv_strs).size()) {
                aliases.push_back((yyvsp[-5].sv_strs)[i+1]);
            } else {
                aliases.push_back("");
            }
        }
        auto stmt = std::make_shared<SelectStmt>((yyvsp[-7].sv_cols), tables, aliases, std::move(conds), (yyvsp[-1].sv_orderby), (yyvsp[-3].sv_cols), (yyvsp[-2].sv_conds), (yyvsp[0].sv_int));
        (yyval.sv_node) = stmt;
    }
#line 1942 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 29: /* dml: EXPLAIN ANALYZE SELECT selector FROM tableRefs optWhereClause opt_group_by opt_having opt_order_clause opt_limit  */
#line 209 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto conds = (yyvsp[-4].sv_conds);
        for (auto &cond : g_on_conds) {
            conds.push_back(cond);
        }
        g_on_conds.clear();
        std::vector<std::string> tables;
        std::vector<std::string> aliases;
        for (size_t i = 0; i < (yyvsp[-5].sv_strs).size(); i += 2) {
            tables.push_back((yyvsp[-5].sv_strs)[i]);
            if (i + 1 < (yyvsp[-5].sv_strs).size()) {
                aliases.push_back((yyvsp[-5].sv_strs)[i+1]);
            } else {
                aliases.push_back("");
            }
        }
        auto select = std::make_shared<SelectStmt>((yyvsp[-7].sv_cols), tables, aliases, std::move(conds), (yyvsp[-1].sv_orderby), (yyvsp[-3].sv_cols), (yyvsp[-2].sv_conds), (yyvsp[0].sv_int));
        select->is_explain_analyze = true;
        (yyval.sv_node) = select;
    }
#line 1967 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 30: /* selectWithin: SELECT selector FROM tableRefs optWhereClause opt_group_by opt_having  */
#line 234 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto conds = (yyvsp[-2].sv_conds);
        for (auto &cond : g_on_conds) {
            conds.push_back(cond);
        }
        g_on_conds.clear();
        std::vector<std::string> tables;
        std::vector<std::string> aliases;
        for (size_t i = 0; i < (yyvsp[-3].sv_strs).size(); i += 2) {
            tables.push_back((yyvsp[-3].sv_strs)[i]);
            if (i + 1 < (yyvsp[-3].sv_strs).size()) {
                aliases.push_back((yyvsp[-3].sv_strs)[i+1]);
            } else {
                aliases.push_back("");
            }
        }
        auto stmt = std::make_shared<SelectStmt>((yyvsp[-5].sv_cols), tables, aliases, std::move(conds), nullptr, (yyvsp[-1].sv_cols), (yyvsp[0].sv_conds), -1);
        (yyval.sv_node) = std::static_pointer_cast<TreeNode>(stmt);
    }
#line 1991 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 31: /* unionQuery: selectWithin UNION selectWithin  */
#line 257 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto u = std::make_shared<UnionStmt>();
        u->selects.push_back(std::static_pointer_cast<SelectStmt>((yyvsp[-2].sv_node)));
        u->selects.push_back(std::static_pointer_cast<SelectStmt>((yyvsp[0].sv_node)));
        (yyval.sv_node) = std::static_pointer_cast<TreeNode>(u);
    }
#line 2002 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 32: /* unionQuery: unionQuery UNION selectWithin  */
#line 264 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto u = std::static_pointer_cast<UnionStmt>((yyvsp[-2].sv_node));
        u->selects.push_back(std::static_pointer_cast<SelectStmt>((yyvsp[0].sv_node)));
        (yyval.sv_node) = (yyvsp[-2].sv_node);
    }
#line 2012 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 33: /* fieldList: field  */
#line 273 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_fields) = std::vector<std::shared_ptr<Field>>{(yyvsp[0].sv_field)};
    }
#line 2020 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 34: /* fieldList: fieldList ',' field  */
#line 277 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_fields).push_back((yyvsp[0].sv_field));
    }
#line 2028 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 35: /* colNameList: colName  */
#line 284 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_strs) = std::vector<std::string>{(yyvsp[0].sv_str)};
    }
#line 2036 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 36: /* colNameList: colNameList ',' colName  */
#line 288 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_strs).push_back((yyvsp[0].sv_str));
    }
#line 2044 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 37: /* field: colName type  */
#line 295 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_field) = std::make_shared<ColDef>((yyvsp[-1].sv_str), (yyvsp[0].sv_type_len));
    }
#line 2052 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 38: /* type: INT  */
#line 302 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_type_len) = std::make_shared<TypeLen>(SV_TYPE_INT, sizeof(int));
    }
#line 2060 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 39: /* type: CHAR '(' VALUE_INT ')'  */
#line 306 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_type_len) = std::make_shared<TypeLen>(SV_TYPE_STRING, (yyvsp[-1].sv_int));
    }
#line 2068 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 40: /* type: FLOAT  */
#line 310 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_type_len) = std::make_shared<TypeLen>(SV_TYPE_FLOAT, sizeof(float));
    }
#line 2076 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 41: /* valueList: value  */
#line 317 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_vals) = std::vector<std::shared_ptr<Value>>{(yyvsp[0].sv_val)};
    }
#line 2084 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 42: /* valueList: valueList ',' value  */
#line 321 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_vals).push_back((yyvsp[0].sv_val));
    }
#line 2092 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 43: /* value: VALUE_INT  */
#line 328 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_val) = std::make_shared<IntLit>((yyvsp[0].sv_int));
    }
#line 2100 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 44: /* value: VALUE_FLOAT  */
#line 332 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_val) = std::make_shared<FloatLit>((yyvsp[0].sv_float));
    }
#line 2108 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 45: /* value: VALUE_STRING  */
#line 336 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_val) = std::make_shared<StringLit>((yyvsp[0].sv_str));
    }
#line 2116 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 46: /* value: VALUE_BOOL  */
#line 340 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_val) = std::make_shared<BoolLit>((yyvsp[0].sv_bool));
    }
#line 2124 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 47: /* condition: col op expr  */
#line 347 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_cond) = std::make_shared<BinaryExpr>((yyvsp[-2].sv_col), (yyvsp[-1].sv_comp_op), (yyvsp[0].sv_expr));
    }
#line 2132 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 48: /* optWhereClause: %empty  */
#line 353 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
                      { (yyval.sv_conds) = std::vector<std::shared_ptr<BinaryExpr>>(); }
#line 2138 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 49: /* optWhereClause: WHERE whereClause  */
#line 355 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_conds) = (yyvsp[0].sv_conds);
    }
#line 2146 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 50: /* whereClause: condition  */
#line 362 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_conds) = std::vector<std::shared_ptr<BinaryExpr>>{(yyvsp[0].sv_cond)};
    }
#line 2154 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 51: /* whereClause: whereClause AND condition  */
#line 366 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_conds).push_back((yyvsp[0].sv_cond));
    }
#line 2162 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 52: /* col: tbName '.' colName  */
#line 373 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_col) = std::make_shared<Col>((yyvsp[-2].sv_str), (yyvsp[0].sv_str));
    }
#line 2170 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 53: /* col: tbName '.' colName AS IDENTIFIER  */
#line 377 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto c = std::make_shared<Col>((yyvsp[-4].sv_str), (yyvsp[-2].sv_str)); c->alias = (yyvsp[0].sv_str); (yyval.sv_col) = c;
    }
#line 2178 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 54: /* col: colName  */
#line 381 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_col) = std::make_shared<Col>("", (yyvsp[0].sv_str));
    }
#line 2186 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 55: /* col: colName AS IDENTIFIER  */
#line 385 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto c = std::make_shared<Col>("", (yyvsp[-2].sv_str)); c->alias = (yyvsp[0].sv_str); (yyval.sv_col) = c;
    }
#line 2194 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 56: /* col: COUNT '(' '*' ')'  */
#line 389 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 0; a->is_star = true;
        auto c = std::make_shared<Col>("", "COUNT(*)"); c->agg = a; (yyval.sv_col) = c;
    }
#line 2203 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 57: /* col: COUNT '(' '*' ')' AS IDENTIFIER  */
#line 394 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 0; a->is_star = true;
        auto c = std::make_shared<Col>("", "COUNT(*)"); c->agg = a; c->alias = (yyvsp[0].sv_str); (yyval.sv_col) = c;
    }
#line 2212 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 58: /* col: COUNT '(' col ')'  */
#line 399 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 0; a->col = (yyvsp[-1].sv_col);
        auto c = std::make_shared<Col>((yyvsp[-1].sv_col)->tab_name, "COUNT(" + (yyvsp[-1].sv_col)->col_name + ")"); c->agg = a; (yyval.sv_col) = c;
    }
#line 2221 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 59: /* col: COUNT '(' col ')' AS IDENTIFIER  */
#line 404 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 0; a->col = (yyvsp[-3].sv_col);
        auto c = std::make_shared<Col>((yyvsp[-3].sv_col)->tab_name, "COUNT(" + (yyvsp[-3].sv_col)->col_name + ")"); c->agg = a; c->alias = (yyvsp[0].sv_str); (yyval.sv_col) = c;
    }
#line 2230 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 60: /* col: MAX '(' col ')'  */
#line 409 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 1; a->col = (yyvsp[-1].sv_col);
        auto c = std::make_shared<Col>((yyvsp[-1].sv_col)->tab_name, "MAX(" + (yyvsp[-1].sv_col)->col_name + ")"); c->agg = a; (yyval.sv_col) = c;
    }
#line 2239 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 61: /* col: MAX '(' col ')' AS IDENTIFIER  */
#line 414 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 1; a->col = (yyvsp[-3].sv_col);
        auto c = std::make_shared<Col>((yyvsp[-3].sv_col)->tab_name, "MAX(" + (yyvsp[-3].sv_col)->col_name + ")"); c->agg = a; c->alias = (yyvsp[0].sv_str); (yyval.sv_col) = c;
    }
#line 2248 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 62: /* col: MIN '(' col ')'  */
#line 419 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 2; a->col = (yyvsp[-1].sv_col);
        auto c = std::make_shared<Col>((yyvsp[-1].sv_col)->tab_name, "MIN(" + (yyvsp[-1].sv_col)->col_name + ")"); c->agg = a; (yyval.sv_col) = c;
    }
#line 2257 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 63: /* col: MIN '(' col ')' AS IDENTIFIER  */
#line 424 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 2; a->col = (yyvsp[-3].sv_col);
        auto c = std::make_shared<Col>((yyvsp[-3].sv_col)->tab_name, "MIN(" + (yyvsp[-3].sv_col)->col_name + ")"); c->agg = a; c->alias = (yyvsp[0].sv_str); (yyval.sv_col) = c;
    }
#line 2266 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 64: /* col: SUM '(' col ')'  */
#line 429 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 3; a->col = (yyvsp[-1].sv_col);
        auto c = std::make_shared<Col>((yyvsp[-1].sv_col)->tab_name, "SUM(" + (yyvsp[-1].sv_col)->col_name + ")"); c->agg = a; (yyval.sv_col) = c;
    }
#line 2275 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 65: /* col: SUM '(' col ')' AS IDENTIFIER  */
#line 434 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 3; a->col = (yyvsp[-3].sv_col);
        auto c = std::make_shared<Col>((yyvsp[-3].sv_col)->tab_name, "SUM(" + (yyvsp[-3].sv_col)->col_name + ")"); c->agg = a; c->alias = (yyvsp[0].sv_str); (yyval.sv_col) = c;
    }
#line 2284 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 66: /* col: AVG '(' col ')'  */
#line 439 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 4; a->col = (yyvsp[-1].sv_col);
        auto c = std::make_shared<Col>((yyvsp[-1].sv_col)->tab_name, "AVG(" + (yyvsp[-1].sv_col)->col_name + ")"); c->agg = a; (yyval.sv_col) = c;
    }
#line 2293 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 67: /* col: AVG '(' col ')' AS IDENTIFIER  */
#line 444 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto a = std::make_shared<AggFunc>(); a->agg_type = 4; a->col = (yyvsp[-3].sv_col);
        auto c = std::make_shared<Col>((yyvsp[-3].sv_col)->tab_name, "AVG(" + (yyvsp[-3].sv_col)->col_name + ")"); c->agg = a; c->alias = (yyvsp[0].sv_str); (yyval.sv_col) = c;
    }
#line 2302 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 68: /* colList: col  */
#line 452 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_cols) = std::vector<std::shared_ptr<Col>>{(yyvsp[0].sv_col)};
    }
#line 2310 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 69: /* colList: colList ',' col  */
#line 456 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_cols).push_back((yyvsp[0].sv_col));
    }
#line 2318 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 70: /* op: '='  */
#line 463 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_EQ;
    }
#line 2326 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 71: /* op: '<'  */
#line 467 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_LT;
    }
#line 2334 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 72: /* op: '>'  */
#line 471 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_GT;
    }
#line 2342 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 73: /* op: NEQ  */
#line 475 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_NE;
    }
#line 2350 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 74: /* op: LEQ  */
#line 479 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_LE;
    }
#line 2358 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 75: /* op: GEQ  */
#line 483 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_GE;
    }
#line 2366 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 76: /* expr: value  */
#line 490 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_expr) = std::static_pointer_cast<Expr>((yyvsp[0].sv_val));
    }
#line 2374 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 77: /* expr: col  */
#line 494 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_expr) = std::static_pointer_cast<Expr>((yyvsp[0].sv_col));
    }
#line 2382 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 78: /* setClauses: setClause  */
#line 501 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_set_clauses) = std::vector<std::shared_ptr<SetClause>>{(yyvsp[0].sv_set_clause)};
    }
#line 2390 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 79: /* setClauses: setClauses ',' setClause  */
#line 505 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_set_clauses).push_back((yyvsp[0].sv_set_clause));
    }
#line 2398 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 80: /* setClause: colName '=' value  */
#line 512 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-2].sv_str), (yyvsp[0].sv_val));
    }
#line 2406 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 81: /* setClause: colName '=' colName '+' value  */
#line 516 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        auto sc = std::make_shared<SetClause>((yyvsp[-4].sv_str), (yyvsp[0].sv_val));
        sc->is_self_ref = true;
        sc->self_ref_col = (yyvsp[-2].sv_str);
        (yyval.sv_set_clause) = sc;
    }
#line 2417 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 82: /* setClause: colName '=' colName '-' value  */
#line 523 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        // Negate the value and treat as addition
        if (auto il = std::dynamic_pointer_cast<ast::IntLit>((yyvsp[0].sv_val))) {
            il->val = -il->val;
        } else if (auto fl = std::dynamic_pointer_cast<ast::FloatLit>((yyvsp[0].sv_val))) {
            fl->val = -fl->val;
        }
        auto sc = std::make_shared<SetClause>((yyvsp[-4].sv_str), (yyvsp[0].sv_val));
        sc->is_self_ref = true;
        sc->self_ref_col = (yyvsp[-2].sv_str);
        (yyval.sv_set_clause) = sc;
    }
#line 2434 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 83: /* selector: '*'  */
#line 539 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_cols) = {};
    }
#line 2442 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 84: /* selector: colList  */
#line 543 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_cols) = (yyvsp[0].sv_cols);
    }
#line 2450 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 85: /* opt_group_by: %empty  */
#line 549 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
                      { (yyval.sv_cols) = std::vector<std::shared_ptr<Col>>(); }
#line 2456 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 86: /* opt_group_by: GROUP BY colList  */
#line 551 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_cols) = (yyvsp[0].sv_cols);
    }
#line 2464 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 87: /* opt_having: %empty  */
#line 557 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
                      { (yyval.sv_conds) = std::vector<std::shared_ptr<BinaryExpr>>(); }
#line 2470 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 88: /* opt_having: HAVING whereClause  */
#line 559 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_conds) = (yyvsp[0].sv_conds);
    }
#line 2478 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 89: /* opt_limit: %empty  */
#line 565 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
                      { (yyval.sv_int) = -1; }
#line 2484 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 90: /* opt_limit: LIMIT VALUE_INT  */
#line 567 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_int) = (yyvsp[0].sv_int);
    }
#line 2492 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 91: /* tableRefs: tableRef  */
#line 575 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_strs) = (yyvsp[0].sv_strs);
    }
#line 2500 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 92: /* tableRefs: tableRefs ',' tableRef  */
#line 579 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_strs).insert((yyval.sv_strs).end(), (yyvsp[0].sv_strs).begin(), (yyvsp[0].sv_strs).end());
    }
#line 2508 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 93: /* tableRefs: tableRefs JOIN tableRef ON whereClause  */
#line 583 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        // 把 ON 条件保存到全局变量中
        for (auto &cond : (yyvsp[0].sv_conds)) {
            g_on_conds.push_back(cond);
        }
        (yyval.sv_strs).insert((yyval.sv_strs).end(), (yyvsp[-2].sv_strs).begin(), (yyvsp[-2].sv_strs).end());
    }
#line 2520 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 94: /* tableRefs: tableRefs JOIN tableRef  */
#line 591 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_strs).insert((yyval.sv_strs).end(), (yyvsp[0].sv_strs).begin(), (yyvsp[0].sv_strs).end());
    }
#line 2528 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 95: /* tableRef: tbName  */
#line 600 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_strs) = std::vector<std::string>{(yyvsp[0].sv_str), ""};
    }
#line 2536 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 96: /* tableRef: tbName tbName  */
#line 604 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        // 表名 + 别名：table_name alias_name
        (yyval.sv_strs) = std::vector<std::string>{(yyvsp[-1].sv_str), (yyvsp[0].sv_str)};
    }
#line 2545 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 97: /* tableRef: tbName AS tbName  */
#line 609 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        // 表名 + AS 别名
        (yyval.sv_strs) = std::vector<std::string>{(yyvsp[-2].sv_str), (yyvsp[0].sv_str)};
    }
#line 2554 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 98: /* tableRef: '(' unionQuery ')' tbName  */
#line 614 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        ast::get_union_map()[(yyvsp[0].sv_str)] = std::static_pointer_cast<UnionStmt>((yyvsp[-2].sv_node));
        (yyval.sv_strs) = std::vector<std::string>{(yyvsp[0].sv_str), (yyvsp[0].sv_str)};
    }
#line 2563 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 99: /* tableRef: '(' unionQuery ')' AS tbName  */
#line 619 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        ast::get_union_map()[(yyvsp[0].sv_str)] = std::static_pointer_cast<UnionStmt>((yyvsp[-3].sv_node));
        (yyval.sv_strs) = std::vector<std::string>{(yyvsp[0].sv_str), (yyvsp[0].sv_str)};
    }
#line 2572 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 100: /* opt_order_clause: ORDER BY order_clause  */
#line 627 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_orderby) = (yyvsp[0].sv_orderby);
    }
#line 2580 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 101: /* opt_order_clause: %empty  */
#line 630 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
                      { (yyval.sv_orderby) = nullptr; }
#line 2586 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 102: /* order_clause: col opt_asc_desc  */
#line 635 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyval.sv_orderby) = std::make_shared<OrderBy>((yyvsp[-1].sv_col), (yyvsp[0].sv_orderby_dir));
    }
#line 2594 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 103: /* order_clause: order_clause ',' col opt_asc_desc  */
#line 639 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
    {
        (yyvsp[-3].sv_orderby)->append((yyvsp[-1].sv_col), (yyvsp[0].sv_orderby_dir));
        (yyval.sv_orderby) = (yyvsp[-3].sv_orderby);
    }
#line 2603 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 104: /* opt_asc_desc: ASC  */
#line 646 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
                 { (yyval.sv_orderby_dir) = OrderBy_ASC;     }
#line 2609 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 105: /* opt_asc_desc: DESC  */
#line 647 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
                 { (yyval.sv_orderby_dir) = OrderBy_DESC;    }
#line 2615 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 106: /* opt_asc_desc: %empty  */
#line 648 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
            { (yyval.sv_orderby_dir) = OrderBy_DEFAULT; }
#line 2621 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 107: /* set_knob_type: ENABLE_NESTLOOP  */
#line 652 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
                    { (yyval.sv_setKnobType) = EnableNestLoop; }
#line 2627 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;

  case 108: /* set_knob_type: ENABLE_SORTMERGE  */
#line 653 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"
                         { (yyval.sv_setKnobType) = EnableSortMerge; }
#line 2633 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"
    break;


#line 2637 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.tab.cpp"

      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", YY_CAST (yysymbol_kind_t, yyr1[yyn]), &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;

  *++yyvsp = yyval;
  *++yylsp = yyloc;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYSYMBOL_YYEMPTY : YYTRANSLATE (yychar);
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
      {
        yypcontext_t yyctx
          = {yyssp, yytoken, &yylloc};
        char const *yymsgp = YY_("syntax error");
        int yysyntax_error_status;
        yysyntax_error_status = yysyntax_error (&yymsg_alloc, &yymsg, &yyctx);
        if (yysyntax_error_status == 0)
          yymsgp = yymsg;
        else if (yysyntax_error_status == -1)
          {
            if (yymsg != yymsgbuf)
              YYSTACK_FREE (yymsg);
            yymsg = YY_CAST (char *,
                             YYSTACK_ALLOC (YY_CAST (YYSIZE_T, yymsg_alloc)));
            if (yymsg)
              {
                yysyntax_error_status
                  = yysyntax_error (&yymsg_alloc, &yymsg, &yyctx);
                yymsgp = yymsg;
              }
            else
              {
                yymsg = yymsgbuf;
                yymsg_alloc = sizeof yymsgbuf;
                yysyntax_error_status = YYENOMEM;
              }
          }
        yyerror (&yylloc, yymsgp);
        if (yysyntax_error_status == YYENOMEM)
          YYNOMEM;
      }
    }

  yyerror_range[1] = yylloc;
  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval, &yylloc);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;
  ++yynerrs;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYSYMBOL_YYerror;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYSYMBOL_YYerror)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;

      yyerror_range[1] = *yylsp;
      yydestruct ("Error: popping",
                  YY_ACCESSING_SYMBOL (yystate), yyvsp, yylsp);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  yyerror_range[2] = yylloc;
  ++yylsp;
  YYLLOC_DEFAULT (*yylsp, yyerror_range, 2);

  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", YY_ACCESSING_SYMBOL (yyn), yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturnlab;


/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturnlab;


/*-----------------------------------------------------------.
| yyexhaustedlab -- YYNOMEM (memory exhaustion) comes here.  |
`-----------------------------------------------------------*/
yyexhaustedlab:
  yyerror (&yylloc, YY_("memory exhausted"));
  yyresult = 2;
  goto yyreturnlab;


/*----------------------------------------------------------.
| yyreturnlab -- parsing is finished, clean up and return.  |
`----------------------------------------------------------*/
yyreturnlab:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval, &yylloc);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  YY_ACCESSING_SYMBOL (+*yyssp), yyvsp, yylsp);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif
  if (yymsg != yymsgbuf)
    YYSTACK_FREE (yymsg);
  return yyresult;
}

#line 659 "/home/administrator1218/db2026-main/rmdb/src/parser/yacc.y"

