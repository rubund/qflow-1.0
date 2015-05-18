/*--------------------------------------------------------------*/
/* liberty2tech.c ---						*/
/*								*/
/*	This program converts a liberty timing file into the	*/
/*	"genlib" format used by ABC for standard cell mapping,	*/
/*	and the "gate.cfg" file used by the BDNetFanout program	*/
/*	for load balancing and delay minimization		*/
/*--------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
 
#define LIB_LINE_MAX  65535

int libCurrentLine;

#define INIT		0
#define LIBBLOCK	1
#define CELLDEF		2
#define PINDEF		3
#define TIMING		4

// Pin types
#define	UNKNOWN		-1
#define INPUT		0
#define OUTPUT		1

// Function translation
#define GROUPBEGIN	1
#define GROUPEND	2
#define SIGNAL		3
#define OPERATOR	4
#define XOPERATOR	5
#define SEPARATOR	6

/*--------------------------------------------------------------*/
/* Database							*/
/*--------------------------------------------------------------*/

typedef struct _lutable *lutableptr;

typedef struct _lutable {
    char *name;
    char invert;	// 0 if times x caps, 1 if caps x times
    char *var1;		// Name of array in index1
    char *var2;		// Name of array in index2
    int  tsize;		// Number of entries in time array
    int  csize;		// Number of entries in cap array
    double *times;	// Time array (units fF)
    double *caps;	// Cap array (units ps)
    lutableptr next;
} lutable;

typedef struct _pin *pinptr;

typedef struct _pin {
    char *name;
    int	type;
    double cap;
    double maxtrans;
    double maxcap;
    pinptr next;
} pin;

typedef struct _cell *cellptr;

typedef struct _cell {
    char *name;
    char *function;
    pin	 *pins;
    double area;
    double slope;
    double mintrans;
    lutable *reftable;
    double *times;	// Local values for time indexes, if given
    double *caps;	// Local values for cap indexes, if given
    double *values;	// Matrix of all values
    cellptr next;
} cell;

/*--------------------------------------------------------------*/
/* Grab a token from the input					*/
/* Return the token, or NULL if we have reached end-of-file.	*/
/*--------------------------------------------------------------*/

char *
advancetoken(FILE *flib, char delimiter)
{
    static char token[LIB_LINE_MAX];
    static char line[LIB_LINE_MAX];
    static char *linepos = NULL;

    char *lineptr = linepos;
    char *lptr, *tptr;
    char *result;
    int commentblock, concat, nest;

    commentblock = 0;
    concat = 0;
    nest = 0;
    while (1) {		/* Keep processing until we get a token or hit EOF */

	if (lineptr != NULL && *lineptr == '/' && *(lineptr + 1) == '*') {
	    commentblock = 1;
	}

	if (commentblock == 1) {
	    if ((lptr = strstr(lineptr, "*/")) != NULL) {
		lineptr = lptr + 2;
		commentblock = 0;
	    }
	    else lineptr = NULL;
	}

	if (lineptr == NULL || *lineptr == '\n' || *lineptr == '\0') {
	    result = fgets(line, LIB_LINE_MAX, flib);
	    libCurrentLine++;
	    if (result == NULL) return NULL;

	    /* Keep pulling stuff in if the line ends with a continuation character */
 	    lptr = line;
	    while (*lptr != '\n' && *lptr != '\0') {
		if (*lptr == '\\') {
		    result = fgets(lptr, LIB_LINE_MAX - (lptr - line), flib);
		    libCurrentLine++;
		    if (result == NULL) break;
		}
		else
		    lptr++;
	    }	
	    if (result == NULL) return NULL;
	    lineptr = line;
	}

	if (commentblock == 1) continue;

	while (isspace(*lineptr)) lineptr++;
	if (concat == 0)
	    tptr = token;

	// Find the next token and return just the token.  Update linepos
	// to the position just beyond the token.  All delimiters like
	// parentheses, quotes, etc., are returned as single tokens

	// If delimiter is declared, then we stop when we reach the
	// delimiter character, and return all the text preceding it
	// as the token.  If delimiter is 0, then we look for standard
	// delimiters, and separate them out and return them as tokens
	// if found.

	while (1) {
	    if (*lineptr == '\n' || *lineptr == '\0')
		break;
	    if (*lineptr == '/' && *(lineptr + 1) == '*')
		break;
	    if (delimiter != 0 && *lineptr == delimiter) {
		if (nest > 0)
		    nest--;
		else
		    break;
	    }

	    // Watch for nested delimiters!
	    if (delimiter == '}' && *lineptr == '{') nest++;
	    if (delimiter == ')' && *lineptr == '(') nest++;

	    if (delimiter == 0)
		if (*lineptr == ' ' || *lineptr == '\t')
		    break;

	    if (delimiter == 0) {
		if (*lineptr == '(' || *lineptr == ')') {
		    if (tptr == token) *tptr++ = *lineptr++;
		    break;
		}
		if (*lineptr == '{' || *lineptr == '}') {
		    if (tptr == token) *tptr++ = *lineptr++;
		    break;
		}
		if (*lineptr == '\"' || *lineptr == ':' || *lineptr == ';') {
		    if (tptr == token) *tptr++ = *lineptr++;
		    break;
		}
	    }

	    *tptr++ = *lineptr++;
	}
	*tptr = '\0';
	if ((delimiter != 0) && (*lineptr != delimiter))
	    concat = 1;
	else if ((delimiter != 0) && (*lineptr == delimiter))
	    break;
	else if (tptr > token)
	    break;
    }
    if (delimiter != 0) lineptr++;

    while (isspace(*lineptr)) lineptr++;
    linepos = lineptr;

    // Final:  Remove trailing whitespace
    tptr = token + strlen(token) - 1;
    while (isspace(*tptr)) {
	*tptr = '\0';
	tptr--;
    }
    return token;
}

/*--------------------------------------------------------------*/
/* Expansion of XOR operator "^" into and/or/invert		*/
/*--------------------------------------------------------------*/

char *
xor_expand(char *lib_func)
{
    static char newfunc[16384];
    char savfunc[16384];
    char *xptr, *sptr, *fptr, *rest, *start;
    int nest, lhsnests, rhsnests;
    char *rhs = NULL;
    char *lhs = NULL;

    strcpy(newfunc, lib_func);

    while ((xptr = strchr(newfunc, '^')) != NULL) {

       /* find expression on RHS */
       sptr = xptr + 1;
       while (*sptr == ' ' || *sptr == '\t') sptr++;
       fptr = sptr;
       rhsnests = 0;
       if (*sptr == '(') {
	  rhsnests = 1;
	  nest = 1;
	  while ((*sptr != ')') || (nest > 0)) {
	     sptr++;
	     if (*sptr == '(') nest++;
	     if (*sptr == ')') nest--;
	  }
       }
       else {
	  while (*sptr != ' ' && *sptr != '\t' && *sptr != '\0' &&
			*sptr != ')')
	     sptr++;
	  if (*sptr == ')') sptr--;
       }

       // If argument is a single character, then don't bother with parentheses
       if (sptr - fptr == 0) rhsnests = 1;

       if (rhsnests == 1) {
          rhs = (char *)malloc(sptr - fptr + 2);
          strncpy(rhs, fptr, sptr - fptr + 1);
	  *(rhs + (int)(sptr - fptr + 1)) = '\0';
       }
       else {
	  /* Add parentheses around RHS */
          rhs = (char *)malloc(sptr - fptr + 4);
	  *rhs = '(';
          strncpy(rhs + 1, fptr, sptr - fptr + 1);
	  *(rhs + (int)(sptr - fptr + 2)) = ')';
	  *(rhs + (int)(sptr - fptr + 3)) = '\0';
       }
       rest = sptr + 1;
       
       /* find expression on LHS */
       sptr = xptr - 1;
       while (*sptr == ' ' || *sptr == '\t') sptr--;
       fptr = sptr;
       lhsnests = 0;
       if (*sptr == ')') {
          lhsnests = 1;
	  nest = 1;
	  while ((*sptr != '(') || (nest > 0)) {
	     sptr--;
	     if (*sptr == ')') nest++;
	     if (*sptr == '(') nest--;
	  }
       }
       else {
	  while (*sptr != ' ' && *sptr != '\t' && sptr != lib_func &&
			*sptr != '(')
	     sptr--;
	  if (*sptr == '(') sptr++;
       }

       // If argument is a single character, then don't bother with parentheses
       if (fptr - sptr == 0) lhsnests = 1;

       if (lhsnests == 1) {
          lhs = (char *)malloc(fptr - sptr + 2);
          strncpy(lhs, sptr, fptr - sptr + 1);
	  *(lhs + (int)(fptr - sptr + 1)) = '\0';
       }
       else {
          lhs = (char *)malloc(fptr - sptr + 4);
	  *lhs = '(';
          strncpy(lhs + 1, sptr, fptr - sptr + 1);
	  *(lhs + (int)(fptr - sptr + 2)) = ')';
	  *(lhs + (int)(fptr - sptr + 3)) = '\0';
       }
       strcpy(savfunc, newfunc);
       start = savfunc + (sptr - newfunc);

       sprintf(start, "(%s*!%s + !%s*%s) %s",
		lhs, rhs, lhs, rhs, rest);

	if (rhs != NULL) free(rhs);
	if (lhs != NULL) free(lhs);

	strcpy(newfunc, savfunc);
    }

    return newfunc;
}

/*--------------------------------------------------------------*/
/* Turn a liberty-format function string into one recognized by	*/
/* genlib.  This means, for the most part, replacing forms of	*/
/* "A B" with "A * B", and "A ^ B" with "A * !B + !A * B"	*/
/*--------------------------------------------------------------*/

char *
get_function(char *out_name, char *lib_func)
{
    static char newfunc[16384];
    char *fptr, *sptr;
    int nest;
    int state = INIT;

    fptr = newfunc;
    sptr = out_name;

    while (*sptr != '\0') *fptr++ = *sptr++;
    *fptr++ = ' ';
    *fptr++ = '=';
    *fptr++ = ' ';

    sptr = xor_expand(lib_func); // genlib can't handle "^"

    while (*sptr != '\0') {
	if (*sptr == '(') {
	    if (state == SIGNAL || state == GROUPEND) {
		*fptr++ = '*';	// Implicit AND;  genlib wants to see
		*fptr++ = ' ';	// this written out explicitly.
	    }	   
	    state = GROUPBEGIN;
	    *fptr++ = *sptr++;
	}
	else if (*sptr == ')') {
	    state = GROUPEND;
	    *fptr++ = *sptr++;
	}
	else if (*sptr == '!' || *sptr == '*' || *sptr == '+' || *sptr == '\'') {
	    state = OPERATOR;
	    *fptr++ = *sptr++;
	}
	else if (*sptr == ' ' || *sptr == '\t') {
	    if (state == SIGNAL) {
		state = SEPARATOR;
	    }
	    *fptr++ = *sptr++;
	}
	else if (*sptr == '^') {
	    state = XOPERATOR;
	}
	else {
	    if (state == SEPARATOR || state == GROUPEND) {
		*fptr++ = '*';	// Implicit AND;  genlib wants to see
		*fptr++ = ' ';	// this written out explicitly.
	    }
	    state = SIGNAL;
	    *fptr++ = *sptr++;
	}
    }
    *fptr = '\0';

    // Process single-quote-as-inversion.  That is, A' --> !A
    // The number of characters remains the same, so we can apply
    // the changes directly to newfunc with careful use of memmove

    while ((sptr = strchr(newfunc, '\'')) != NULL) {
	fptr = sptr - 1;
	while (isspace(*fptr)) fptr--;

	if (*fptr == ')') {
	    nest = 1;
	    while (nest > 0) {
		fptr--;
		if (*fptr == ')') nest++;
		else if (*fptr == '(') nest--;
		else if (fptr == newfunc) break;
	    }
	}
	else {
	    while (*fptr != '!' && *fptr != '*' && *fptr != '+' &&
		   !isspace(*fptr) && (fptr > newfunc) && *fptr != '('
		   && *fptr != ')')
		fptr--;
	    if (fptr > newfunc) fptr++;
	}
	memmove(fptr + 1, fptr, (size_t)(sptr - fptr));
	*fptr = '!';
    }

    return newfunc;
}

/*--------------------------------------------------------------*/
/* Name pattern matching.  This is used to restrict the 	*/
/* entries that are placed in genlib.  It understands a few	*/
/* wildcard characters: "^" (matches beginning-of-string), and	*/
/* "$" (matches end-of-string).					*/
/*								*/
/* May want to add "|" and "&" (OR, AND) functions, but maybe	*/
/* it's not necessary.  Also standard wildcards like "." and	*/
/* "*".								*/
/*--------------------------------------------------------------*/

int
pattern_match(char *name, char *pattern)
{
    char *sptr;
    int plen = strlen(pattern);
    int rval = 0;
    int matchend = 0;

    if (*(pattern + plen - 1) == '$') {
	matchend = 1;
	*(pattern + plen - 1) = '\0';
    }

    if (*pattern == '^') {
	sptr = pattern + 1;
	if (matchend) {
	    if (!strcmp(name, sptr))
		rval = 1;
	    else
		rval = 0;
	}
	else {
	    if (!strncmp(name, sptr, plen - 2))
		rval = 1;
	    else
		rval = 0;
	}
    }
    else {
	if (matchend) {
	    sptr = name + strlen(name) - plen + 1;
	    if (!strcmp(sptr, pattern))
		rval = 1;
	    else
		rval = 0;
	}
	else {
	    if (strstr(name, pattern) != NULL)
		rval = 1;
	    else
		rval = 0;
	}
    }

    if (matchend) *(pattern + plen - 1) = '$';
    return rval;
}

/*--------------------------------------------------------------*/
/* Main program							*/
/*--------------------------------------------------------------*/

int
main(int objc, char *argv[])
{
    FILE *flib;
    FILE *fgen;
    FILE *fcfg;
    char *token;
    char *libname = NULL;
    int section = INIT;
    lutable *tables = NULL;
    cell *cells = NULL;

    double time_unit = 1.0;	// Time unit multiplier, to get ps
    double cap_unit = 1.0;	// Capacitive unit multiplier, to get fF

    int i, j;
    double gval;
    char *iptr;

    lutable *newtable, *reftable;
    cell *newcell, *lastcell;
    pin *newpin, *lastpin;
    char *curfunc, *pattern = NULL;

    if (objc != 4 && objc != 5) {
	fprintf(stderr, "Usage:  liberty2tech <name.lib> <name.genlib> <gate.cfg>"
		" [<pattern>]\n");
	exit (1);
    }
    if (objc == 5) pattern = strdup(argv[4]);

    flib = fopen(argv[1], "r");
    if (flib == NULL) {
	fprintf(stderr, "Cannot open %s for reading\n", argv[1]);
	exit (1);
    }

    /* Read the file.  This is not a rigorous parser! */

    libCurrentLine = 0;
    lastcell = NULL;

    /* Read tokens off of the line */
    token = advancetoken(flib, 0);

    while (token != NULL) {

	switch (section) {
	    case INIT:
		if (!strcasecmp(token, "library")) {
		    token = advancetoken(flib, 0);
		    if (strcmp(token, "("))
			fprintf(stderr, "Library not followed by name\n");
		    else
			token = advancetoken(flib, ')');
		    fprintf(stderr, "Parsing library \"%s\"\n", token);
		    libname = strdup(token);
		    token = advancetoken(flib, 0);
		    if (strcmp(token, "{")) {
			fprintf(stderr, "Did not find opening brace "
					"on library block\n");
			exit(1);
		    }
		    section = LIBBLOCK;
		}
		else
		    fprintf(stderr, "Unknown input \"%s\", looking for "
					"\"library\"\n", token);
		break;

	    case LIBBLOCK:
		// Here we check for the main blocks, again not rigorously. . .

		if (!strcasecmp(token, "}")) {
		    fprintf(stdout, "End of library at line %d\n", libCurrentLine);
		    section = INIT;			// End of library block
		}
		else if (!strcasecmp(token, "delay_model")) {
		    token = advancetoken(flib, 0);
		    if (strcmp(token, ":"))
			fprintf(stderr, "Input missing colon\n");
		    token = advancetoken(flib, ';');
		    if (strcasecmp(token, "table_lookup")) {
			fprintf(stderr, "Sorry, only know how to "
					"handle table lookup!\n");
			exit(1);
		    }
		}
		else if (!strcasecmp(token, "lu_table_template")) {
		    // Read in template information;
		    newtable = (lutable *)malloc(sizeof(lutable));
		    newtable->var1 = NULL;
		    newtable->var2 = NULL;
		    newtable->tsize = 0;
		    newtable->csize = 0;
		    newtable->times = NULL;
		    newtable->caps = NULL;
		    newtable->next = tables;
		    tables = newtable;

		    token = advancetoken(flib, 0);
		    if (strcmp(token, "("))
			fprintf(stderr, "Input missing open parens\n");
		    else
			token = advancetoken(flib, ')');
		    newtable->name = strdup(token);
		    while (*token != '}') {
			token = advancetoken(flib, 0);
			if (!strcasecmp(token, "variable_1")) {
			    token = advancetoken(flib, 0);
			    token = advancetoken(flib, ';');
			    newtable->var1 = strdup(token);
			    if (strstr(token, "capacitance") != NULL)
				newtable->invert = 1;
			}
			else if (!strcasecmp(token, "variable_2")) {
			    token = advancetoken(flib, 0);
			    token = advancetoken(flib, ';');
			    newtable->var2 = strdup(token);
			    if (strstr(token, "transition") != NULL)
				newtable->invert = 1;
			}
			else if (!strcasecmp(token, "index_1")) {
			    token = advancetoken(flib, 0);	// Open parens
			    token = advancetoken(flib, 0);	// Quote
			    if (!strcmp(token, "\""))
				token = advancetoken(flib, '\"');

			    if (newtable->invert == 1) {
				// Count entries
				iptr = token;
				newtable->csize = 1;
				while ((iptr = strchr(iptr, ',')) != NULL) {
				    iptr++;
				    newtable->csize++;
				}
				newtable->caps = (double *)malloc(newtable->csize *
					sizeof(double));
				newtable->csize = 0;
				iptr = token;
				sscanf(iptr, "%lg", &newtable->caps[0]);
				newtable->caps[0] *= cap_unit;
				while ((iptr = strchr(iptr, ',')) != NULL) {
				    iptr++;
				    newtable->csize++;
				    sscanf(iptr, "%lg",
						&newtable->caps[newtable->csize]);
				    newtable->caps[newtable->csize] *= cap_unit;
				}
				newtable->csize++;
			    }
			    else {	// newtable->invert = 0
				// Count entries
				iptr = token;
				newtable->tsize = 1;
				while ((iptr = strchr(iptr, ',')) != NULL) {
				    iptr++;
				    newtable->tsize++;
				}
				newtable->times = (double *)malloc(newtable->tsize *
					sizeof(double));
				newtable->tsize = 0;
				iptr = token;
				sscanf(iptr, "%lg", &newtable->times[0]);
				newtable->times[0] *= time_unit; 
				while ((iptr = strchr(iptr, ',')) != NULL) {
				    iptr++;
				    newtable->tsize++;
				    sscanf(iptr, "%lg",
						&newtable->times[newtable->tsize]);
				    newtable->times[newtable->tsize] *= time_unit;
				}
				newtable->tsize++;
			    }

			    token = advancetoken(flib, ';'); // EOL semicolon
			}
			else if (!strcasecmp(token, "index_2")) {
			    token = advancetoken(flib, 0);	// Open parens
			    token = advancetoken(flib, 0);	// Quote
			    if (!strcmp(token, "\""))
				token = advancetoken(flib, '\"');

			    if (newtable->invert == 0) {
				// Count entries
				iptr = token;
				newtable->csize = 1;
				while ((iptr = strchr(iptr, ',')) != NULL) {
				    iptr++;
				    newtable->csize++;
				}
				newtable->caps = (double *)malloc(newtable->csize *
					sizeof(double));
				newtable->csize = 0;
				iptr = token;
				sscanf(iptr, "%lg", &newtable->caps[0]);
				newtable->caps[0] *= cap_unit;
				while ((iptr = strchr(iptr, ',')) != NULL) {
				    iptr++;
				    newtable->csize++;
				    sscanf(iptr, "%lg",
						&newtable->caps[newtable->csize]);
				    newtable->caps[newtable->csize] *= cap_unit;
				}
				newtable->csize++;
			    }
			    else { 	// newtable->invert == 1
				// Count entries
				iptr = token;
				newtable->tsize = 1;
				while ((iptr = strchr(iptr, ',')) != NULL) {
				    iptr++;
				    newtable->tsize++;
				}
				newtable->times = (double *)malloc(newtable->tsize *
					sizeof(double));
				newtable->tsize = 0;
				iptr = token;
				sscanf(iptr, "%lg", &newtable->times[0]);
				newtable->times[0] *= time_unit;
				while ((iptr = strchr(iptr, ',')) != NULL) {
				    iptr++;
				    newtable->tsize++;
				    sscanf(iptr, "%lg",
						&newtable->times[newtable->tsize]);
				    newtable->times[newtable->tsize] *= time_unit;
				}
				newtable->tsize++;
			    }

			    token = advancetoken(flib, ';'); // EOL semicolon
			}
		    }
		}
		else if (!strcasecmp(token, "cell")) {
		    newcell = (cell *)malloc(sizeof(cell));
		    newcell->next = NULL;
		    if (lastcell != NULL)
			lastcell->next = newcell;
		    else
			cells = newcell;
		    lastcell = newcell;
		    token = advancetoken(flib, 0);	// Open parens
		    if (!strcmp(token, "("))
			token = advancetoken(flib, ')');	// Cellname
		    newcell->name = strdup(token);
		    token = advancetoken(flib, 0);	// Find start of block
		    if (strcmp(token, "{"))
			fprintf(stderr, "Error: failed to find start of block\n");
		    newcell->reftable = NULL;
		    newcell->function = NULL;
		    newcell->pins = NULL;
		    newcell->area = 1.0;
		    newcell->slope = 1.0;
		    newcell->mintrans = 0.0;
		    newcell->times = NULL;
		    newcell->caps = NULL;
		    newcell->values = NULL;
		    lastpin = NULL;
		    section = CELLDEF;
		}
		else if (!strcasecmp(token, "time_unit")) {
		   char *metric;

		   token = advancetoken(flib, 0);
		   if (token == NULL) break;
		   if (!strcmp(token, ":")) {
		      token = advancetoken(flib, 0);
		      if (token == NULL) break;
		   }
		   if (!strcmp(token, "\"")) {
		      token = advancetoken(flib, '\"');
		      if (token == NULL) break;
		   }
		   time_unit = strtod(token, &metric);
		   if (*metric != '\0') {
		      if (!strcmp(metric, "ns"))
			 time_unit *= 1E3;
		      else if (!strcmp(metric, "us"))
			 time_unit *= 1E6;
		      else if (!strcmp(metric, "fs"))
			 time_unit *= 1E-3;
		      else if (strcmp(metric, "ps"))
			 fprintf(stderr, "Don't understand time units \"%s\"\n",
				token);
		   }
		   else {
		      token = advancetoken(flib, 0);
		      if (token == NULL) break;
		      if (!strcmp(token, "ns"))
			 time_unit *= 1E3;
		      else if (!strcmp(token, "us"))
			 time_unit *= 1E6;
		      else if (!strcmp(token, "fs"))
			 time_unit *= 1E-3;
		      else if (strcmp(token, "ps"))
			 fprintf(stderr, "Don't understand time units \"%s\"\n",
				token);
		   }
		   token = advancetoken(flib, ';');
		}
		else if (!strcasecmp(token, "capacitive_load_unit")) {
		   char *metric;

		   token = advancetoken(flib, 0);
		   if (token == NULL) break;
		   if (!strcmp(token, "(")) {
		      token = advancetoken(flib, ')');
		      if (token == NULL) break;
		   }
		   cap_unit = strtod(token, &metric);
		   if (*metric != '\0') {
		      while (isspace(*metric)) metric++;
		      if (*metric == ',') metric++;
		      while ((*metric != '\0') && isspace(*metric)) metric++;
		      if (!strcasecmp(metric, "af"))
			 cap_unit *= 1E-3;
		      else if (!strcasecmp(metric, "pf"))
			 cap_unit *= 1000;
		      else if (!strcasecmp(metric, "nf"))
			 cap_unit *= 1E6;
		      else if (!strcasecmp(metric, "uf"))
			 cap_unit *= 1E9;
		      else if (strcasecmp(metric, "ff"))
			 fprintf(stderr, "Don't understand capacitive units \"%s\"\n",
				token);
		   }
		   else {
		      token = advancetoken(flib, 0);
		      if (token == NULL) break;
		      if (!strcasecmp(token, "af"))
			 cap_unit *= 1E-3;
		      else if (!strcasecmp(token, "pf"))
			 cap_unit *= 1000;
		      else if (!strcasecmp(token, "nf"))
			 cap_unit *= 1E6;
		      else if (!strcasecmp(token, "uf"))
			 cap_unit *= 1E9;
		      else if (strcasecmp(token, "ff"))
			 fprintf(stderr, "Don't understand capacitive units \"%s\"\n",
				token);
		   }
		   token = advancetoken(flib, ';');
		}
		else {
		    // For unhandled tokens, read in tokens.  If it is
		    // a definition or function, read to end-of-line.  If
		    // it is a block definition, read to end-of-block.
		    while (1) {
			token = advancetoken(flib, 0);
			if (token == NULL) break;
			if (!strcmp(token, ";")) break;
			if (!strcmp(token, "\""))
			    token = advancetoken(flib, '\"');
			if (!strcmp(token, "{")) {
			    token = advancetoken(flib, '}');
			    break;
			}
		    }
		}
		break;

	    case CELLDEF:

		if (!strcmp(token, "}")) {
		    section = LIBBLOCK;			// End of cell def
		}
		else if (!strcasecmp(token, "pin")) {
		    token = advancetoken(flib, 0);	// Open parens
		    if (!strcmp(token, "("))
			token = advancetoken(flib, ')');	// Close parens
		    newpin = (pin *)malloc(sizeof(pin));
		    newpin->name = strdup(token);

		    newpin->next = NULL;
		    if (lastpin != NULL)
			lastpin->next = newpin;
		    else
			newcell->pins = newpin;
		    lastpin = newpin;

		    token = advancetoken(flib, 0);	// Find start of block
		    if (strcmp(token, "{"))
			fprintf(stderr, "Error: failed to find start of block\n");
		    newpin->type = UNKNOWN;
		    newpin->cap = 0.0;
		    newpin->maxcap = 0.0;
		    newpin->maxtrans = 0.0;
		    section = PINDEF;
		}		
		else if (!strcasecmp(token, "area")) {
		    token = advancetoken(flib, 0);	// Colon
		    token = advancetoken(flib, ';');	// To end-of-statement
		    sscanf(token, "%lg", &newcell->area);
		}
		else {
		    // For unhandled tokens, read in tokens.  If it is
		    // a definition or function, read to end-of-line.  If
		    // it is a block definition, read to end-of-block.
		    while (1) {
			token = advancetoken(flib, 0);
			if (token == NULL) break;
			if (!strcmp(token, ";")) break;
			if (!strcmp(token, "\""))
			    token = advancetoken(flib, '\"');
			if (!strcmp(token, "{")) {
			    token = advancetoken(flib, '}');
			    break;
			}
		    }
		}
		break;

	    case PINDEF:

		if (!strcmp(token, "}")) {
		    section = CELLDEF;			// End of pin def
		}
		else if (!strcasecmp(token, "capacitance")) {
		    token = advancetoken(flib, 0);	// Colon
		    token = advancetoken(flib, ';');	// To end-of-statement
		    sscanf(token, "%lg", &newpin->cap);
		    newpin->cap *= cap_unit;
		}
		else if (!strcasecmp(token, "function")) {
		    token = advancetoken(flib, 0);	// Colon
		    token = advancetoken(flib, 0);	// Open quote
		    if (!strcmp(token, "\""))
			token = advancetoken(flib, '\"');	// Find function string
		    if (newpin->type == OUTPUT) {
			char *rfunc = get_function(newpin->name, token);
			newcell->function = strdup(rfunc);
		    }
		    token = advancetoken(flib, 0);
		    if (strcmp(token, ";"))
			fprintf(stderr, "Expected end-of-statement.\n");
		}
		else if (!strcasecmp(token, "direction")) {
		    token = advancetoken(flib, 0);	// Colon
		    token = advancetoken(flib, ';');
		    if (!strcasecmp(token, "input")) {
			newpin->type = INPUT;
		    }
		    else if (!strcasecmp(token, "output")) {
			newpin->type = OUTPUT;
		    }
		}
		else if (!strcasecmp(token, "max_transition")) {
		    token = advancetoken(flib, 0);	// Colon
		    token = advancetoken(flib, ';');	// To end-of-statement
		    sscanf(token, "%lg", &newpin->maxtrans);
		    newpin->maxtrans *= time_unit;
		}
		else if (!strcasecmp(token, "max_capacitance")) {
		    token = advancetoken(flib, 0);	// Colon
		    token = advancetoken(flib, ';');	// To end-of-statement
		    sscanf(token, "%lg", &newpin->maxcap);
		    newpin->maxcap *= cap_unit;
		}
		else if (!strcasecmp(token, "timing")) {
		    token = advancetoken(flib, 0);	// Arguments, if any
		    if (strcmp(token, "("))
			fprintf(stderr, "Error: failed to find start of block\n");
		    else
		       token = advancetoken(flib, ')');	// Arguments, if any
		    token = advancetoken(flib, 0);	// Find start of block
		    if (strcmp(token, "{"))
			fprintf(stderr, "Error: failed to find start of block\n");
		    section = TIMING;
		}
		else {
		    // For unhandled tokens, read in tokens.  If it is
		    // a definition or function, read to end-of-line.  If
		    // it is a block definition, read to end-of-block.
		    while (1) {
			token = advancetoken(flib, 0);
			if (token == NULL) break;
			if (!strcmp(token, ";")) break;
			if (!strcmp(token, "\""))
			    token = advancetoken(flib, '\"');
			if (!strcmp(token, "{")) {
			    token = advancetoken(flib, '}');
			    break;
			}
		    }
		}
		break;

	    case TIMING:

		if (!strcmp(token, "}")) {
		    section = PINDEF;			// End of timing def
		}
		else if (!strcasecmp(token, "cell_rise")) {
		    token = advancetoken(flib, 0);	// Open parens
		    if (!strcmp(token, "("))
			token = advancetoken(flib, ')');
		    if (strcmp(token, "scalar")) {
			
		        for (reftable = tables; reftable; reftable = reftable->next)
			    if (!strcmp(reftable->name, token))
			        break;
		        if (reftable == NULL)
			    fprintf(stderr, "Failed to find a valid table \"%s\"\n",
				    token);
		        else if (newcell->reftable == NULL)
			    newcell->reftable = reftable;
		    }

		    token = advancetoken(flib, 0);
		    if (strcmp(token, "{"))
			fprintf(stderr, "Failed to find start of cell_rise block\n");

		    while (*token != '}') {
		        token = advancetoken(flib, 0);
		        if (!strcasecmp(token, "index_1")) {

			    // Local index values override those in the template

			    token = advancetoken(flib, 0);	// Open parens
			    token = advancetoken(flib, 0);	// Quote
			    if (!strcmp(token, "\""))
				token = advancetoken(flib, '\"');

			    //-------------------------

			    if (reftable && (reftable->invert == 1)) {
				// Entries had better match the ref table
				iptr = token;
				i = 0;
				newcell->caps = (double *)malloc(reftable->csize *
					sizeof(double));
				sscanf(iptr, "%lg", &newcell->caps[0]);
				newcell->caps[0] *= cap_unit;
				while ((iptr = strchr(iptr, ',')) != NULL) {
				    iptr++;
				    i++;
				    sscanf(iptr, "%lg", &newcell->caps[i]);
				    newcell->caps[i] *= cap_unit;
				}
			    }
			    else if (reftable && (reftable->invert == 0)) {
				iptr = token;
				i = 0;
				newcell->times = (double *)malloc(reftable->tsize *
					sizeof(double));
				sscanf(iptr, "%lg", &newcell->times[0]);
				newcell->times[0] *= time_unit;
				while ((iptr = strchr(iptr, ',')) != NULL) {
				    iptr++;
				    i++;
				    sscanf(iptr, "%lg", &newcell->times[i]);
				    newcell->times[i] *= time_unit;
				}
			    }

			    token = advancetoken(flib, ')'); 	// Close paren
			    token = advancetoken(flib, ';');	// EOL semicolon
			}
		        else if (!strcasecmp(token, "index_2")) {

			    // Local index values override those in the template

			    token = advancetoken(flib, 0);	// Open parens
			    token = advancetoken(flib, 0);	// Quote
			    if (!strcmp(token, "\""))
				token = advancetoken(flib, '\"');

			    //-------------------------

			    if (reftable && (reftable->invert == 1)) {
				// Entries had better match the ref table
				iptr = token;
				i = 0;
				newcell->times = (double *)malloc(reftable->tsize *
					sizeof(double));
				sscanf(iptr, "%lg", &newcell->times[0]);
				newcell->times[0] *= time_unit;
				while ((iptr = strchr(iptr, ',')) != NULL) {
				    iptr++;
				    i++;
				    sscanf(iptr, "%lg", &newcell->times[i]);
				    newcell->times[i] *= time_unit;
				}
			    }
			    else if (reftable && (reftable->invert == 0)) {
				iptr = token;
				i = 0;
				newcell->caps = (double *)malloc(reftable->csize *
					sizeof(double));
				sscanf(iptr, "%lg", &newcell->caps[0]);
				newcell->caps[0] *= cap_unit;
				while ((iptr = strchr(iptr, ',')) != NULL) {
				    iptr++;
				    i++;
				    sscanf(iptr, "%lg", &newcell->caps[i]);
				    newcell->caps[i] *= cap_unit;
				}
			    }

			    token = advancetoken(flib, ')'); 	// Close paren
			    token = advancetoken(flib, ';');	// EOL semicolon
			}
			else if (!strcasecmp(token, "values")) {
			    token = advancetoken(flib, 0);	
			    if (strcmp(token, "("))
				fprintf(stderr, "Failed to find start of"
						" value table\n");
			    token = advancetoken(flib, ')');

			    // Parse the string of values and enter it into the
			    // table "values", which is size csize x tsize

			    if (reftable && reftable->csize > 0 && reftable->tsize > 0) {
				if (reftable->invert) {
				    newcell->values = (double *)malloc(reftable->csize *
						reftable->tsize * sizeof(double));
				    iptr = token;
				    for (i = 0; i < reftable->tsize; i++) {
					for (j = 0; j < reftable->csize; j++) {
					    while (*iptr == ' ' || *iptr == '\"' ||
							*iptr == ',')
						iptr++;
					    sscanf(iptr, "%lg", &gval);
					    *(newcell->values + j * reftable->tsize
							+ i) = gval * time_unit;
					    while (*iptr != ' ' && *iptr != '\"' &&
							*iptr != ',')
						iptr++;
					}
				    }
				}
				else {
				    newcell->values = (double *)malloc(reftable->csize *
						reftable->tsize * sizeof(double));
				    iptr = token;
				    for (j = 0; j < reftable->csize; j++) {
					for (i = 0; i < reftable->tsize; i++) {
					    while (*iptr == ' ' || *iptr == '\"' ||
							*iptr == ',')
						iptr++;
					    sscanf(iptr, "%lg", &gval);
					    *(newcell->values + j * reftable->tsize
							+ i) = gval * time_unit;
					    while (*iptr != ' ' && *iptr != '\"' &&
							*iptr != ',')
						iptr++;
					}
				    }
				}
			    }

			    token = advancetoken(flib, 0);
			    if (strcmp(token, ";"))
				fprintf(stderr, "Failed to find end of value table\n");
			    token = advancetoken(flib, 0);

			}
			else if (strcmp(token, "{"))
			    fprintf(stderr, "Failed to find end of timing block\n");
		    }
		}
		else {
		    // For unhandled tokens, read in tokens.  If it is
		    // a definition or function, read to end-of-line.  If
		    // it is a block definition, read to end-of-block.
		    while (1) {
			token = advancetoken(flib, 0);
			if (token == NULL) break;
			if (!strcmp(token, ";")) break;
			if (!strcmp(token, "\""))
			    token = advancetoken(flib, '\"');
			if (!strcmp(token, "{")) {
			    token = advancetoken(flib, '}');
			    break;
			}
		    }
		}
		break;
	}
	token = advancetoken(flib, 0);
    }
    fprintf(stdout, "Lib Read:  Processed %d lines.\n", libCurrentLine);

    if (flib != NULL) fclose(flib);

    /* Temporary:  Print information gathered */

/*-------------------------------------------------------------------
    for (newtable = tables; newtable; newtable = newtable->next) {
	fprintf(stdout, "Table: %s\n", newtable->name);
    }

    for (newcell = cells; newcell; newcell = newcell->next) {
	fprintf(stdout, "Cell: %s\n", newcell->name);
	fprintf(stdout, "   Function: %s\n", newcell->function);
	for (newpin = newcell->pins; newpin; newpin = newpin->next) {
	    if (newpin->type == INPUT)
		fprintf(stdout, "   Pin: %s  cap=%g\n", newpin->name, newpin->cap);
	}
	fprintf(stdout, "\n");
    }
-------------------------------------------------------------------*/

    /* Now generate the output files */

    /* ----------- */

    fcfg = fopen(argv[3], "w");
    if (fcfg == NULL) {
	fprintf(stderr, "Cannot open %s for reading\n", argv[3]);
	exit (1);
    }

    fprintf(fcfg, "# comments begin with #\n\n");
    fprintf(fcfg, "# Format is propagation delay with internal and pin capacitances.\n");
    fprintf(fcfg, "# Only format D0 is supported for now.\n");
    fprintf(fcfg, "FORMAT D0\n\n");
    fprintf(fcfg, "#----------------------------------------------------------------\n");
    fprintf(fcfg, "# Gate drive strength information for library %s\n",
			(libname == NULL) ? "" : libname);
    fprintf(fcfg, "#----------------------------------------------------------------\n");
    fprintf(fcfg, "# \"delay\" is propagation delay in ps/fF of load capacitance\n");
    fprintf(fcfg, "# \"Cint\", \"Cin1\", ... are all in fF.\n");
    fprintf(fcfg, "#----------------------------------------------------------------\n");
    fprintf(fcfg, "# This file generated by liberty2tech\n\n");
    fprintf(fcfg, "# gatename delay num_inputs Cint Cpin1 Cpin2...\n\n");

    for (newcell = cells; newcell; newcell = newcell->next) {
	int i, cellinputs;
 	double *times, *caps;
	char *tptr, *eptr;
	double mintrans, mincap, maxcap, mintrise, maxtrise;
	double loaddelay, intcap;

	// If this cell does not have a timing table or timing values, ignore it.
	if (newcell->reftable == NULL || newcell->values == NULL) continue;

	// Count the number of input pins on the cell

	cellinputs = 0;
	for (newpin = newcell->pins; newpin; newpin = newpin->next) {
	    if (newpin->type == INPUT)
		cellinputs++;
	}

	if (newcell->times != NULL)
	    times = newcell->times;
	else
	    times = newcell->reftable->times;

	if (newcell->caps != NULL)
	    caps = newcell->caps;
	else
	    caps = newcell->reftable->caps;

	// Find the smallest value in the input net transition table.
	// Assume it is the first value, therefore we want to parse the
	// first row of the cell table.  If that's not true, then we need
	// to add more sophisticated parsing code here!

	mintrans = *times;

	// Find the smallest and largest values in the output net capacitance table

	mincap = *caps;
	maxcap = *(caps + newcell->reftable->csize - 1);

	// Pick up values for rise time under maximum and minimum loads in
	// the template.

	mintrise = *newcell->values;
	maxtrise = *(newcell->values + newcell->reftable->csize - 1);

	// Calculate delay per load.  Note that cap at this point should be
	// in fF, and trise should be in ps.
	// So the value of loaddelay is in ps/fF.
	loaddelay = (maxtrise - mintrise) / (maxcap - mincap);
	newcell->slope = loaddelay;
	newcell->mintrans = mintrise;

	// Calculate internal capacitance
	// risetime is ps, so (risetime / loaddelay) is fF.
	// mincap is in fF.
	intcap = (mintrise / loaddelay) - mincap;

	// Print out all values so far.
	fprintf(fcfg, "%s  %g %d %g  ", newcell->name, loaddelay,
		cellinputs, intcap);

	// Finally, print out the capacitances for each cell, in fF

	for (newpin = newcell->pins; newpin; newpin = newpin->next) {
	    if (newpin->type == INPUT)
		fprintf(fcfg, " %g", newpin->cap);
	}
	fprintf(fcfg, "\n");
    }

    fprintf(fcfg, "# end of gate.cfg\n");
    fclose(fcfg);

    /* ----------- */

    fgen = fopen(argv[2], "w");
    if (fgen == NULL) {
	fprintf(stderr, "Cannot open %s for reading\n", argv[2]);
	exit (1);
    }

    fprintf(fgen, "# Genlib file created by liberty2tech\n");
    if (libname != NULL) 
	fprintf(fgen, "# from library %s\n", libname);
    fprintf(fgen, "\n");
    
    curfunc = NULL;
    for (newcell = cells; newcell; newcell = newcell->next) {

	/* Cells without functions cannot be listed */

	if (newcell->function == NULL) continue;

	/* If no pattern was given, then we try to ensure that	*/
	/* each entry has a unique logic function.  This is not	*/
	/* very reliable, and should be improved.		*/

	if (pattern != NULL) {
	    if (!pattern_match(newcell->name, pattern))
	 	continue;
	}
	else if ((curfunc != NULL) && !strcmp(newcell->function, curfunc))
	    continue;

	curfunc = newcell->function;
	fprintf(fgen, "GATE %s %g %s;\n", newcell->name,
	    newcell->area, newcell->function);

	/* Units are:  cap in pF, maxload in pF, risetime in ns,	*/
	/* slope in ns/pF, falltime in ns, slope in ns/pF.		*/


	for (newpin = newcell->pins; newpin; newpin = newpin->next) {
	    if (newpin->maxcap == 0.0) {
		if (newpin->maxtrans != 0.0)
		    newpin->maxcap = (newpin->maxtrans / newcell->slope);
		else { 
		    pin *testpin;

		    // If any pin has maximum capacitance information, use that.
		    for (testpin = newpin->next; testpin; testpin = testpin->next)
			if (testpin->maxcap != 0.0)
			    newpin->maxcap = testpin->maxcap;

		    // If any pin has maximum transition information, use that.
		    if (newpin->maxcap == 0.0) {
			for (testpin = newpin->next; testpin; testpin = testpin->next)
			    if (testpin->maxtrans != 0.0)
				newpin->maxcap = (testpin->maxtrans / newcell->slope);
		    }

		    // No information on maximum load at all?  Ad hoc rule,
		    // use 24 * cap value of 1st pin;  e.g., a load of about
		    // 24 gates.
		    if (newpin->maxcap == 0.0)
			newpin->maxcap = 24 * newcell->pins->cap;
		}
	    }

	    if (newpin->type == INPUT)
		fprintf(fgen, "   PIN %s %s %g %g %g %g %g %g\n",
			newpin->name, "UNKNOWN", (newpin->cap / 1000.0),
			(newpin->maxcap / 1000.0),
			(newcell->mintrans / 1000.0), newcell->slope,
			(newcell->mintrans / 1000.0), newcell->slope);
	}
	fprintf(fgen, "\n");
    }
    fclose(fgen);

    /* ----------- */

    return 0;
}