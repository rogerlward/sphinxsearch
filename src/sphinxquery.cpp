//
// $Id$
//

//
// Copyright (c) 2001-2012, Andrew Aksyonoff
// Copyright (c) 2008-2012, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "sphinx.h"
#include "sphinxquery.h"
#include "sphinxutils.h"
#include <stdarg.h>

//////////////////////////////////////////////////////////////////////////
// EXTENDED PARSER RELOADED
//////////////////////////////////////////////////////////////////////////

#include "yysphinxquery.h"

// #define XQDEBUG 1
// #define XQ_DUMP_TRANSFORMED_TREE 1
// #define XQ_DUMP_NODE_ADDR 1

//////////////////////////////////////////////////////////////////////////

class XQParser_t
{
public:
					XQParser_t ();
					~XQParser_t ();

public:
	bool			Parse ( XQQuery_t & tQuery, const char * sQuery, const ISphTokenizer * pTokenizer, const CSphSchema * pSchema, CSphDict * pDict, const CSphIndexSettings & tSettings );

	bool			Error ( const char * sTemplate, ... ) __attribute__ ( ( format ( printf, 2, 3 ) ) );
	void			Warning ( const char * sTemplate, ... ) __attribute__ ( ( format ( printf, 2, 3 ) ) );

	bool			AddField ( CSphSmallBitvec & dFields, const char * szField, int iLen );
	bool			ParseFields ( CSphSmallBitvec & uFields, int & iMaxFieldPos, bool & bIgnore );
	int				ParseZone ( const char * pZone );

	bool			IsSpecial ( char c );
	int				GetToken ( YYSTYPE * lvalp );

	void			AddQuery ( XQNode_t * pNode );
	XQNode_t *		AddKeyword ( const char * sKeyword, DWORD uStar = STAR_NONE );
	XQNode_t *		AddKeyword ( XQNode_t * pLeft, XQNode_t * pRight );
	XQNode_t *		AddOp ( XQOperator_e eOp, XQNode_t * pLeft, XQNode_t * pRight, int iOpArg=0 );

	void			Cleanup ();
	XQNode_t *		SweepNulls ( XQNode_t * pNode );
	bool			FixupNots ( XQNode_t * pNode );
	void			DeleteNodesWOFields ( XQNode_t * pNode );

	inline void SetFieldSpec ( const CSphSmallBitvec& uMask, int iMaxPos )
	{
		FixRefSpec();
		m_dStateSpec.Last()->SetFieldSpec ( uMask, iMaxPos );
	}
	inline void SetZoneVec ( int iZoneVec, bool bZoneSpan = false )
	{
		FixRefSpec();
		m_dStateSpec.Last()->SetZoneSpec ( m_dZoneVecs[iZoneVec], bZoneSpan );
	}

	inline void FixRefSpec ()
	{
		bool bRef = ( m_dStateSpec.GetLength()>1 && ( m_dStateSpec[m_dStateSpec.GetLength()-1]==m_dStateSpec[m_dStateSpec.GetLength()-2] ) );
		if ( !bRef )
			return;

		XQLimitSpec_t * pSpec = m_dStateSpec.Pop();
		m_dSpecPool.Add ( new XQLimitSpec_t ( *pSpec ) );
		m_dStateSpec.Add ( m_dSpecPool.Last() );
	}

public:
	const CSphVector<int> & GetZoneVec ( int iZoneVec ) const
	{
		return m_dZoneVecs[iZoneVec];
	}

public:
	XQQuery_t *				m_pParsed;

	BYTE *					m_sQuery;
	int						m_iQueryLen;
	const char *			m_pLastTokenStart;

	const CSphSchema *		m_pSchema;
	ISphTokenizer *			m_pTokenizer;
	CSphDict *				m_pDict;

	const char *			m_pCur;

	CSphVector<XQNode_t*>	m_dSpawned;
	XQNode_t *				m_pRoot;

	bool					m_bStopOnInvalid;
	int						m_iAtomPos;

	int						m_iPendingNulls;
	int						m_iPendingType;
	YYSTYPE					m_tPendingToken;
	bool					m_bWasBlended;

	bool					m_bEmpty;
	bool					m_bQuoted;
	bool					m_bEmptyStopword;
	int						m_iOvershortStep;

	CSphVector<CSphString>	m_dIntTokens;

	CSphVector < CSphVector<int> >	m_dZoneVecs;
	CSphVector<XQLimitSpec_t *>		m_dStateSpec;
	CSphVector<XQLimitSpec_t *>		m_dSpecPool;
};

//////////////////////////////////////////////////////////////////////////

int yylex ( YYSTYPE * lvalp, XQParser_t * pParser )
{
	return pParser->GetToken ( lvalp );
}

void yyerror ( XQParser_t * pParser, const char * sMessage )
{
	if ( pParser->m_pParsed->m_sParseError.IsEmpty() )
		pParser->m_pParsed->m_sParseError.SetSprintf ( "%s near '%s'", sMessage, pParser->m_pLastTokenStart );
}

#include "yysphinxquery.c"

//////////////////////////////////////////////////////////////////////////

void XQLimitSpec_t::SetFieldSpec ( const CSphSmallBitvec& uMask, int iMaxPos )
{
	m_bFieldSpec = true;
	m_dFieldMask = uMask;
	m_iFieldMaxPos = iMaxPos;
}

/// ctor
XQNode_t::XQNode_t ( const XQLimitSpec_t & dSpec )
: m_pParent ( NULL )
, m_eOp ( SPH_QUERY_AND )
, m_iOrder ( 0 )
, m_iCounter ( 0 )
, m_iMagicHash ( 0 )
, m_iFuzzyHash ( 0 )
, m_dSpec ( dSpec )
, m_iOpArg ( 0 )
, m_iAtomPos ( -1 )
, m_iUser ( 0 )
, m_bVirtuallyPlain ( false )
, m_bNotWeighted ( false )
, m_bPercentOp ( false )
{
#ifdef XQ_DUMP_NODE_ADDR
	printf ( "node new 0x%08x\n", this );
#endif
}

/// dtor
XQNode_t::~XQNode_t ()
{
#ifdef XQ_DUMP_NODE_ADDR
	printf ( "node deleted %d 0x%08x\n", m_eOp, this );
#endif
	ARRAY_FOREACH ( i, m_dChildren )
		SafeDelete ( m_dChildren[i] );
}


void XQNode_t::SetFieldSpec ( const CSphSmallBitvec& uMask, int iMaxPos )
{
	// set it, if we do not yet have one
	if ( !m_dSpec.m_bFieldSpec )
		m_dSpec.SetFieldSpec ( uMask, iMaxPos );

	// some of the children might not yet have a spec, even if the node itself has
	// eg. 'hello @title world' (whole node has '@title' spec but 'hello' node does not have any!)
	ARRAY_FOREACH ( i, m_dChildren )
		m_dChildren[i]->SetFieldSpec ( uMask, iMaxPos );
}

void XQLimitSpec_t::SetZoneSpec ( const CSphVector<int> & dZones, bool bZoneSpan )
{
	m_dZones = dZones;
	m_bZoneSpan = bZoneSpan;
}


void XQNode_t::SetZoneSpec ( const CSphVector<int> & dZones, bool bZoneSpan )
{
	// set it, if we do not yet have one
	if ( !m_dSpec.m_dZones.GetLength() )
		m_dSpec.SetZoneSpec ( dZones, bZoneSpan );

	// some of the children might not yet have a spec, even if the node itself has
	ARRAY_FOREACH ( i, m_dChildren )
		m_dChildren[i]->SetZoneSpec ( dZones, bZoneSpan );
}

void XQNode_t::CopySpecs ( const XQNode_t * pSpecs )
{
	if ( !pSpecs )
		return;

	if ( !m_dSpec.m_bFieldSpec )
		m_dSpec.SetFieldSpec ( pSpecs->m_dSpec.m_dFieldMask, pSpecs->m_dSpec.m_iFieldMaxPos );

	if ( !m_dSpec.m_dZones.GetLength() )
		m_dSpec.SetZoneSpec ( pSpecs->m_dSpec.m_dZones, pSpecs->m_dSpec.m_bZoneSpan );
}


void XQNode_t::ClearFieldMask ()
{
	m_dSpec.m_dFieldMask.Set();

	ARRAY_FOREACH ( i, m_dChildren )
		m_dChildren[i]->ClearFieldMask();
}


bool XQNode_t::IsEqualTo ( const XQNode_t * pNode )
{
	if ( !pNode || pNode->GetHash()!=GetHash() || pNode->GetOp()!=GetOp() )
		return false;

	if ( m_dWords.GetLength() )
	{
		// two plain nodes. let's compare the keywords
		if ( pNode->m_dWords.GetLength()!=m_dWords.GetLength() )
			return false;

		if ( !m_dWords.GetLength() )
			return true;

		SmallStringHash_T<int> hSortedWords;
		ARRAY_FOREACH ( i, pNode->m_dWords )
			hSortedWords.Add ( 0, pNode->m_dWords[i].m_sWord );

		ARRAY_FOREACH ( i, m_dWords )
			if ( !hSortedWords.Exists ( m_dWords[i].m_sWord ) )
				return false;

		return true;
	}

	// two non-plain nodes. let's compare the children
	if ( pNode->m_dChildren.GetLength()!=m_dChildren.GetLength() )
		return false;

	if ( !m_dChildren.GetLength() )
		return true;

	ARRAY_FOREACH ( i, m_dChildren )
		if ( !pNode->m_dChildren[i]->IsEqualTo ( m_dChildren[i] ) )
			return false;
	return true;
}


uint64_t XQNode_t::GetHash () const
{
	if ( m_iMagicHash )
		return m_iMagicHash;

	XQOperator_e dZeroOp[2];
	dZeroOp[0] = m_eOp;
	dZeroOp[1] = (XQOperator_e) 0;

	ARRAY_FOREACH ( i, m_dWords )
		m_iMagicHash = 100 + ( m_iMagicHash ^ sphFNV64 ( (const BYTE*)m_dWords[i].m_sWord.cstr() ) ); ///< +100 to make it non-transitive
	ARRAY_FOREACH ( j, m_dChildren )
		m_iMagicHash = 100 + ( m_iMagicHash ^ m_dChildren[j]->GetHash() ); ///< +100 to make it non-transitive
	m_iMagicHash += 1000000; ///< to immerse difference between parents and children
	m_iMagicHash ^= sphFNV64 ( (const BYTE*)dZeroOp );

	return m_iMagicHash;
}


uint64_t XQNode_t::GetFuzzyHash () const
{
	if ( m_iFuzzyHash )
		return m_iFuzzyHash;

	XQOperator_e dZeroOp[2];
	dZeroOp[0] = ( m_eOp==SPH_QUERY_PHRASE ? SPH_QUERY_PROXIMITY : m_eOp );
	dZeroOp[1] = (XQOperator_e) 0;

	ARRAY_FOREACH ( i, m_dWords )
		m_iFuzzyHash = 100 + ( m_iFuzzyHash ^ sphFNV64 ( (const BYTE*)m_dWords[i].m_sWord.cstr() ) ); ///< +100 to make it non-transitive
	ARRAY_FOREACH ( j, m_dChildren )
		m_iFuzzyHash = 100 + ( m_iFuzzyHash ^ m_dChildren[j]->GetFuzzyHash () ); ///< +100 to make it non-transitive
	m_iFuzzyHash += 1000000; ///< to immerse difference between parents and children
	m_iFuzzyHash ^= sphFNV64 ( (const BYTE*)dZeroOp );

	return m_iFuzzyHash;
}


void XQNode_t::SetOp ( XQOperator_e eOp, XQNode_t * pArg1, XQNode_t * pArg2 )
{
	m_eOp = eOp;
	m_dChildren.Reset();
	if ( pArg1 )
	{
		m_dChildren.Add ( pArg1 );
		pArg1->m_pParent = this;
	}
	if ( pArg2 )
	{
		m_dChildren.Add ( pArg2 );
		pArg2->m_pParent = this;
	}
}


XQNode_t * XQNode_t::Clone ()
{
	XQNode_t * pRet = new XQNode_t ( m_dSpec );
	pRet->SetOp ( m_eOp );
	pRet->m_dWords = m_dWords;
	pRet->m_iOpArg = m_iOpArg;
	pRet->m_iAtomPos = m_iAtomPos;
	pRet->m_bVirtuallyPlain = m_bVirtuallyPlain;
	pRet->m_bNotWeighted = m_bNotWeighted;
	pRet->m_bPercentOp = m_bPercentOp;

	if ( m_dChildren.GetLength()==0 )
		return pRet;

	pRet->m_dChildren.Reserve ( m_dChildren.GetLength() );
	for ( int i = 0; i < m_dChildren.GetLength(); ++i )
	{
		pRet->m_dChildren.Add ( m_dChildren[i]->Clone() );
		pRet->m_dChildren.Last()->m_pParent = pRet;
	}

	return pRet;
}


bool XQNode_t::ResetHash ()
{
	bool bAlreadyReset = ( m_iMagicHash==0 && m_iFuzzyHash==0 );
	m_iMagicHash = 0;
	m_iFuzzyHash = 0;
	return bAlreadyReset;
}


/// return either index of pNode among Parent.m_dChildren, or -1
static int GetNodeChildIndex ( const XQNode_t * pParent, const XQNode_t * pNode )
{
	assert ( pParent && pNode );
	ARRAY_FOREACH ( i, pParent->m_dChildren )
		if ( pParent->m_dChildren[i]==pNode )
			return i;

	return -1;
}

//////////////////////////////////////////////////////////////////////////

XQParser_t::XQParser_t ()
	: m_pParsed ( NULL )
	, m_pLastTokenStart ( NULL )
	, m_pRoot ( NULL )
	, m_bStopOnInvalid ( true )
	, m_bWasBlended ( false )
	, m_bQuoted ( false )
	, m_bEmptyStopword ( false )
{
	m_dSpecPool.Add ( new XQLimitSpec_t() );
	m_dStateSpec.Add ( m_dSpecPool.Last() );
}

XQParser_t::~XQParser_t ()
{
	ARRAY_FOREACH ( i, m_dSpecPool )
		SafeDelete ( m_dSpecPool[i] );
}


/// cleanup spawned nodes (for bailing out on errors)
void XQParser_t::Cleanup ()
{
	m_dSpawned.Uniq(); // FIXME! should eliminate this by testing

	ARRAY_FOREACH ( i, m_dSpawned )
	{
		m_dSpawned[i]->m_dChildren.Reset ();
		SafeDelete ( m_dSpawned[i] );
	}
	m_dSpawned.Reset ();
	m_dStateSpec.Reset();
}



bool XQParser_t::Error ( const char * sTemplate, ... )
{
	assert ( m_pParsed );
	char sBuf[256];

	const char * sPrefix = "query error: ";
	int iPrefix = strlen(sPrefix);
	memcpy ( sBuf, sPrefix, iPrefix );

	va_list ap;
	va_start ( ap, sTemplate );
	vsnprintf ( sBuf+iPrefix, sizeof(sBuf)-iPrefix, sTemplate, ap );
	va_end ( ap );

	m_pParsed->m_sParseError = sBuf;
	return false;
}


void XQParser_t::Warning ( const char * sTemplate, ... )
{
	assert ( m_pParsed );
	char sBuf[256];

	const char * sPrefix = "query warning: ";
	int iPrefix = strlen(sPrefix);
	memcpy ( sBuf, sPrefix, iPrefix );

	va_list ap;
	va_start ( ap, sTemplate );
	vsnprintf ( sBuf+iPrefix, sizeof(sBuf)-iPrefix, sTemplate, ap );
	va_end ( ap );

	m_pParsed->m_sParseWarning = sBuf;
}


/// my special chars
bool XQParser_t::IsSpecial ( char c )
{
	return c=='(' || c==')' || c=='|' || c=='-' || c=='!' || c=='@' || c=='~' || c=='"' || c=='/';
}


/// lookup field and add it into mask
bool XQParser_t::AddField ( CSphSmallBitvec & dFields, const char * szField, int iLen )
{
	CSphString sField;
	sField.SetBinary ( szField, iLen );

	int iField = m_pSchema->GetFieldIndex ( sField.cstr () );
	if ( iField < 0 )
	{
		if ( m_bStopOnInvalid )
			return Error ( "no field '%s' found in schema", sField.cstr () );
		else
			Warning ( "no field '%s' found in schema", sField.cstr () );
	} else
	{
		if ( iField>=SPH_MAX_FIELDS )
			return Error ( " max %d fields allowed", SPH_MAX_FIELDS );

		dFields.Set(iField);
	}

	return true;
}


/// parse fields block
bool XQParser_t::ParseFields ( CSphSmallBitvec & dFields, int & iMaxFieldPos, bool & bIgnore )
{
	dFields.Unset();
	iMaxFieldPos = 0;
	bIgnore = false;

	const char * pPtr = m_pTokenizer->GetBufferPtr ();
	const char * pLastPtr = m_pTokenizer->GetBufferEnd ();

	if ( pPtr==pLastPtr )
		return true; // silently ignore trailing field operator

	bool bNegate = false;
	bool bBlock = false;

	// handle special modifiers
	if ( *pPtr=='!' )
	{
		// handle @! and @!(
		bNegate = true; pPtr++;
		if ( *pPtr=='(' ) { bBlock = true; pPtr++; }

	} else if ( *pPtr=='*' )
	{
		// handle @*
		dFields.Set();
		m_pTokenizer->SetBufferPtr ( pPtr+1 );
		return true;

	} else if ( *pPtr=='(' )
	{
		// handle @(
		bBlock = true; pPtr++;
	}

	// handle invalid chars
	if ( !sphIsAlpha(*pPtr) )
	{
		bIgnore = true;
		m_pTokenizer->SetBufferPtr ( pPtr ); // ignore and re-parse (FIXME! maybe warn?)
		return true;
	}
	assert ( sphIsAlpha(*pPtr) ); // i think i'm paranoid

	// handle field specification
	if ( !bBlock )
	{
		// handle standalone field specification
		const char * pFieldStart = pPtr;
		while ( sphIsAlpha(*pPtr) && pPtr<pLastPtr )
			pPtr++;

		assert ( pPtr-pFieldStart>0 );
		if ( !AddField ( dFields, pFieldStart, pPtr-pFieldStart ) )
			return false;

		m_pTokenizer->SetBufferPtr ( pPtr );
		if ( bNegate )
			dFields.Negate();

	} else
	{
		// handle fields block specification
		assert ( sphIsAlpha(*pPtr) && bBlock ); // and complicated

		bool bOK = false;
		const char * pFieldStart = NULL;
		while ( pPtr<pLastPtr )
		{
			// accumulate field name, while we can
			if ( sphIsAlpha(*pPtr) )
			{
				if ( !pFieldStart )
					pFieldStart = pPtr;
				pPtr++;
				continue;
			}

			// separator found
			if ( pFieldStart==NULL )
			{
				CSphString sContext;
				sContext.SetBinary ( pPtr, (int)( pLastPtr-pPtr ) );
				return Error ( "invalid field block operator syntax near '%s'", sContext.cstr() ? sContext.cstr() : "" );

			} else if ( *pPtr==',' )
			{
				if ( !AddField ( dFields, pFieldStart, pPtr-pFieldStart ) )
					return false;

				pFieldStart = NULL;
				pPtr++;

			} else if ( *pPtr==')' )
			{
				if ( !AddField ( dFields, pFieldStart, pPtr-pFieldStart ) )
					return false;

				m_pTokenizer->SetBufferPtr ( ++pPtr );
				if ( bNegate )
					dFields.Negate();

				bOK = true;
				break;

			} else
			{
				return Error ( "invalid character '%c' in field block operator", *pPtr );
			}
		}
		if ( !bOK )
			return Error ( "missing closing ')' in field block operator" );
	}

	// handle optional position range modifier
	if ( pPtr[0]=='[' && isdigit ( pPtr[1] ) )
	{
		// skip '[' and digits
		const char * p = pPtr+1;
		while ( *p && isdigit(*p) ) p++;

		// check that the range ends with ']' (FIXME! maybe report an error if it does not?)
		if ( *p!=']' )
			return true;

		// fetch my value
		iMaxFieldPos = strtoul ( pPtr+1, NULL, 10 );
		m_pTokenizer->SetBufferPtr ( p+1 );
	}

	// well done
	return true;
}


/// helper find-or-add (make it generic and move to sphinxstd?)
static int GetZoneIndex ( XQQuery_t * pQuery, const CSphString & sZone )
{
	ARRAY_FOREACH ( i, pQuery->m_dZones )
		if ( pQuery->m_dZones[i]==sZone )
			return i;

	pQuery->m_dZones.Add ( sZone );
	return pQuery->m_dZones.GetLength()-1;
}


/// parse zone
int XQParser_t::ParseZone ( const char * pZone )
{
	const char * p = pZone;

	// case one, just a single zone name
	if ( sphIsAlpha ( *pZone ) )
	{
		// find zone name
		while ( sphIsAlpha(*p) )
			p++;
		m_pTokenizer->SetBufferPtr ( p );

		// extract and lowercase it
		CSphString sZone;
		sZone.SetBinary ( pZone, p-pZone );
		sZone.ToLower();

		// register it in zones list
		int iZone = GetZoneIndex ( m_pParsed, sZone );

		// create new 1-zone vector
		m_dZoneVecs.Add().Add ( iZone );
		return m_dZoneVecs.GetLength()-1;
	}

	// case two, zone block
	// it must follow strict (name1,name2,...) syntax
	if ( *pZone=='(' )
	{
		// create new zone vector
		CSphVector<int> & dZones = m_dZoneVecs.Add();
		p = ++pZone;

		// scan names
		for ( ;; )
		{
			// syntax error, name expected!
			if ( !sphIsAlpha(*p) )
			{
				Error ( "unexpected character '%c' in zone block operator", *p );
				return -1;
			}

			// scan next name
			while ( sphIsAlpha(*p) )
				p++;

			// extract and lowercase it
			CSphString sZone;
			sZone.SetBinary ( pZone, p-pZone );
			sZone.ToLower();

			// register it in zones list
			dZones.Add ( GetZoneIndex ( m_pParsed, sZone ) );

			// must be either followed by comma, or closing paren
			// everything else will cause syntax error
			if ( *p==')' )
			{
				m_pTokenizer->SetBufferPtr ( p+1 );
				break;
			}

			if ( *p==',' )
				pZone = ++p;
		}

		return m_dZoneVecs.GetLength()-1;
	}

	// unhandled case
	Error ( "internal error, unhandled case in ParseZone()" );
	return -1;
}


/// a lexer of my own
int XQParser_t::GetToken ( YYSTYPE * lvalp )
{
	// what, noone's pending for a bending?!
	if ( !m_iPendingType )
		for ( ;; )
	{
		assert ( m_iPendingNulls==0 );

		if ( m_bWasBlended )
			m_iAtomPos += m_pTokenizer->SkipBlended();

		// tricky stuff
		// we need to manually check for numbers in certain states (currently, just after proximity or quorum operator)
		// required because if 0-9 are not in charset_table, or min_word_len is too high,
		// the tokenizer will *not* return the number as a token!
		m_pLastTokenStart = m_pTokenizer->GetBufferPtr ();
		const char * sEnd = m_pTokenizer->GetBufferEnd ();

		const char * p = m_pLastTokenStart;
		while ( p<sEnd && isspace ( *(BYTE*)p ) ) p++; // to avoid CRT assertions on Windows

		int iDots = 0;
		const char * sToken = p;
		while ( p<sEnd && ( isdigit ( *(BYTE*)p ) || *p=='.' ) )
		{
			iDots += ( *p=='.' );
			p++;
		}

		// must be float number but got many dots or only dot
		if ( iDots && ( iDots>1 || p-sToken==iDots ) )
			p = sToken;

		static const int NUMBER_BUF_LEN = 10; // max strlen of int32
		if ( p>sToken && p-sToken<NUMBER_BUF_LEN
			&& !( *p=='-' && !( p-sToken==1 && sphIsModifier ( p[-1] ) ) ) // !bDashInside copied over from arbitration
			&& ( *p=='\0' || sphIsSpace(*p) || IsSpecial(*p) ) )
		{
			if ( m_pTokenizer->GetToken() && m_pTokenizer->TokenIsBlended() ) // number with blended should be tokenized as usual
			{
				m_pTokenizer->SkipBlended();
				m_pTokenizer->SetBufferPtr ( m_pLastTokenStart );
			} else
			{
				// got not a very long number followed by a whitespace or special, handle it
				char sNumberBuf[NUMBER_BUF_LEN];

				int iNumberLen = Min ( (int)sizeof(sNumberBuf)-1, int(p-sToken) );
				memcpy ( sNumberBuf, sToken, iNumberLen );
				sNumberBuf[iNumberLen] = '\0';
				if ( iDots )
					m_tPendingToken.tInt.fValue = (float)strtod ( sNumberBuf, NULL );
				else
					m_tPendingToken.tInt.iValue = atoi ( sNumberBuf );

				// check if it can be used as a keyword too
				m_pTokenizer->SetBuffer ( (BYTE*)sNumberBuf, iNumberLen );
				sToken = (const char*) m_pTokenizer->GetToken();
				m_pTokenizer->SetBuffer ( m_sQuery, m_iQueryLen );
				m_pTokenizer->SetBufferPtr ( p );

				m_tPendingToken.tInt.iStrIndex = -1;
				if ( sToken )
				{
					m_dIntTokens.Add ( sToken );
					if ( m_pDict->GetWordID ( (BYTE*)sToken ) )
						m_tPendingToken.tInt.iStrIndex = m_dIntTokens.GetLength()-1;
					else
						m_dIntTokens.Pop();
					m_iAtomPos++;
				}

				m_iPendingNulls = 0;
				m_iPendingType = iDots ? TOK_FLOAT : TOK_INT;
				break;
			}
		}

		// not a number, long number, or number not followed by a whitespace, so fallback to regular tokenizing
		sToken = (const char *) m_pTokenizer->GetToken ();
		if ( !sToken )
		{
			m_iPendingNulls = m_pTokenizer->GetOvershortCount() * m_iOvershortStep;
			if ( !( m_iPendingNulls || m_pTokenizer->GetBufferPtr()-p>0 ) )
				return 0;
			m_iPendingNulls = 0;
			lvalp->pNode = AddKeyword ( NULL );
			return TOK_KEYWORD;
		}

		// now let's do some token post-processing
		m_bWasBlended = m_pTokenizer->TokenIsBlended();
		m_bEmpty = false;

		m_iPendingNulls = m_pTokenizer->GetOvershortCount() * m_iOvershortStep;
		m_iAtomPos += 1+m_iPendingNulls;

		// handle NEAR (must be case-sensitive, and immediately followed by slash and int)
		if ( sToken && p && !m_pTokenizer->m_bPhrase && strncmp ( p, "NEAR/", 5 )==0 && isdigit(p[5]) )
		{
			// extract that int
			int iVal = 0;
			for ( p=p+5; isdigit(*p); p++ )
				iVal = iVal*10 + (*p) - '0'; // FIXME! check for overflow?
			m_pTokenizer->SetBufferPtr ( p );

			// we just lexed our next token
			m_iPendingType = TOK_NEAR;
			m_tPendingToken.tInt.iValue = iVal;
			m_tPendingToken.tInt.iStrIndex = -1;
			m_iAtomPos -= 1; // skip NEAR
			break;
		}

		// handle SENTENCE
		if ( sToken && p && !m_pTokenizer->m_bPhrase && !strcasecmp ( sToken, "sentence" ) && !strncmp ( p, "SENTENCE", 8 ) )
		{
			// we just lexed our next token
			m_iPendingType = TOK_SENTENCE;
			m_iAtomPos -= 1;
			break;
		}

		// handle PARAGRAPH
		if ( sToken && p && !m_pTokenizer->m_bPhrase && !strcasecmp ( sToken, "paragraph" ) && !strncmp ( p, "PARAGRAPH", 9 ) )
		{
			// we just lexed our next token
			m_iPendingType = TOK_PARAGRAPH;
			m_iAtomPos -= 1;
			break;
		}

		// handle ZONE
		if ( sToken && p && !m_pTokenizer->m_bPhrase && !strncmp ( p, "ZONE:", 5 )
			&& ( sphIsAlpha(p[5]) || p[5]=='(' ) )
		{
			// ParseZone() will update tokenizer buffer ptr as needed
			int iVec = ParseZone ( p+5 );
			if ( iVec<0 )
				return -1;

			// we just lexed our next token
			m_iPendingType = TOK_ZONE;
			m_tPendingToken.iZoneVec = iVec;
			m_iAtomPos -= 1;
			break;
		}

		// handle ZONESPAN
		if ( sToken && p && !m_pTokenizer->m_bPhrase && !strncmp ( p, "ZONESPAN:", 9 )
			&& ( sphIsAlpha(p[9]) || p[9]=='(' ) )
		{
			// ParseZone() will update tokenizer buffer ptr as needed
			int iVec = ParseZone ( p+9 );
			if ( iVec<0 )
				return -1;

			// we just lexed our next token
			m_iPendingType = TOK_ZONESPAN;
			m_tPendingToken.iZoneVec = iVec;
			m_iAtomPos -= 1;
			break;
		}

		// handle specials
		if ( m_pTokenizer->WasTokenSpecial() )
		{
			// specials must not affect pos
			m_iAtomPos--;

			// some specials are especially special
			if ( sToken[0]=='@' )
			{
				bool bIgnore;

				// parse fields operator
				if ( !ParseFields ( m_tPendingToken.tFieldLimit.dMask, m_tPendingToken.tFieldLimit.iMaxPos, bIgnore ) )
					return -1;

				if ( bIgnore )
					continue;

				if ( m_pSchema->m_dFields.GetLength()!=SPH_MAX_FIELDS )
					m_tPendingToken.tFieldLimit.dMask.LimitBits ( m_pSchema->m_dFields.GetLength() );

				m_iPendingType = TOK_FIELDLIMIT;
				break;

			} else if ( sToken[0]=='<' )
			{
				if ( *m_pTokenizer->GetBufferPtr()=='<' )
				{
					// got "<<", aka operator BEFORE
					m_iPendingType = TOK_BEFORE;
					break;
				} else
				{
					// got stray '<', ignore
					continue;
				}
			} else
			{
				// all the other specials are passed to parser verbatim
				if ( sToken[0]=='"' )
					m_bQuoted = !m_bQuoted;
				m_iPendingType = sToken[0]=='!' ? '-' : sToken[0];
				m_pTokenizer->m_bPhrase = m_bQuoted;

				if ( sToken[0]=='(' )
				{
					// safe way of performing m_dStateSpec.Add ( m_dStateSpec.Last() )
					m_dStateSpec.Add ();
					m_dStateSpec[m_dStateSpec.GetLength()-1] = m_dStateSpec[m_dStateSpec.GetLength()-2];
				} else if ( sToken[0]==')' && m_dStateSpec.GetLength()>1 )
				{
					m_dStateSpec.Pop();
				}

				break;
			}
		}

		// check for stopword, and create that node
		// temp buffer is required, because GetWordID() might expand (!) the keyword in-place
		const int MAX_BYTES = 3*SPH_MAX_WORD_LEN + 16;
		BYTE sTmp [ MAX_BYTES ];

		strncpy ( (char*)sTmp, sToken, MAX_BYTES );
		sTmp[MAX_BYTES-1] = '\0';

		if ( !m_pDict->GetWordID ( sTmp ) )
		{
			sToken = NULL;
			// stopwords with step=0 must not affect pos
			if ( m_bEmptyStopword )
				m_iAtomPos--;
		}

		// information about stars is lost after this point, so was have to save it now
		DWORD uStarPosition = STAR_NONE;
		uStarPosition |= *m_pTokenizer->GetTokenEnd()=='*' ? STAR_BACK : 0;
		uStarPosition |= ( m_pTokenizer->GetTokenStart()>(const char *)m_sQuery ) &&
			m_pTokenizer->GetTokenStart()[-1]=='*' ? STAR_FRONT : 0;

		m_tPendingToken.pNode = AddKeyword ( sToken, uStarPosition );
		m_iPendingType = TOK_KEYWORD;

		if ( m_pTokenizer->TokenIsBlended() )
			m_iAtomPos--;
		break;
	}

	// someone must be pending now!
	assert ( m_iPendingType );
	m_bEmpty = false;

	// ladies first, though
	if ( m_iPendingNulls>0 )
	{
		m_iPendingNulls--;
		lvalp->pNode = AddKeyword ( NULL );
		return TOK_KEYWORD;
	}

	// pending the offending
	int iRes = m_iPendingType;
	m_iPendingType = 0;

	*lvalp = m_tPendingToken;
	return iRes;
}


void XQParser_t::AddQuery ( XQNode_t * pNode )
{
	m_pRoot = pNode;
}


XQNode_t * XQParser_t::AddKeyword ( const char * sKeyword, DWORD uStarPosition )
{
	XQKeyword_t tAW ( sKeyword, m_iAtomPos );
	tAW.m_uStarPosition = uStarPosition;

	XQNode_t * pNode = new XQNode_t ( *m_dStateSpec.Last() );
	pNode->m_dWords.Add ( tAW );

	m_dSpawned.Add ( pNode );
	return pNode;
}


XQNode_t * XQParser_t::AddKeyword ( XQNode_t * pLeft, XQNode_t * pRight )
{
	if ( !pLeft || !pRight )
		return pLeft ? pLeft : pRight;

	assert ( pLeft->m_dWords.GetLength()>0 );
	assert ( pRight->m_dWords.GetLength()==1 );

	pLeft->m_dWords.Add ( pRight->m_dWords[0] );
	m_dSpawned.RemoveValue ( pRight );
	SafeDelete ( pRight );
	return pLeft;
}


XQNode_t * XQParser_t::AddOp ( XQOperator_e eOp, XQNode_t * pLeft, XQNode_t * pRight, int iOpArg )
{
	/////////
	// unary
	/////////

	if ( eOp==SPH_QUERY_NOT )
	{
		XQNode_t * pNode = new XQNode_t ( *m_dStateSpec.Last() );
		pNode->SetOp ( SPH_QUERY_NOT, pLeft );
		m_dSpawned.Add ( pNode );
		return pNode;
	}

	//////////
	// binary
	//////////

	if ( !pLeft || !pRight )
		return pLeft ? pLeft : pRight;

	// left spec always tries to infect the nodes to the right, only brackets can stop it
	// eg. '@title hello' vs 'world'
	pRight->CopySpecs ( pLeft );

	// build a new node
	XQNode_t * pResult = NULL;
	if ( pLeft->m_dChildren.GetLength() && pLeft->GetOp()==eOp && pLeft->m_iOpArg==iOpArg )
	{
		pLeft->m_dChildren.Add ( pRight );
		pRight->m_pParent = pLeft;
		pResult = pLeft;
		if ( pRight->m_dSpec.m_bFieldSpec )
			pResult->m_dSpec.SetFieldSpec ( pRight->m_dSpec.m_dFieldMask, pRight->m_dSpec.m_iFieldMaxPos );

		if ( pRight->m_dSpec.m_dZones.GetLength() )
			pResult->m_dSpec.SetZoneSpec ( pRight->m_dSpec.m_dZones, pRight->m_dSpec.m_bZoneSpan );
	} else
	{
		// however, it's right (!) spec which is chosen for the resulting node,
		// eg. '@title hello' + 'world @body program'
		XQNode_t * pNode = new XQNode_t ( pRight->m_dSpec );
		pNode->SetOp ( eOp, pLeft, pRight );
		pNode->m_iOpArg = iOpArg;
		m_dSpawned.Add ( pNode );
		pResult = pNode;
	}
	return pResult;
}


XQNode_t * XQParser_t::SweepNulls ( XQNode_t * pNode )
{
	if ( !pNode )
		return NULL;

	// sweep plain node
	if ( pNode->m_dWords.GetLength() )
	{
		ARRAY_FOREACH ( i, pNode->m_dWords )
			if ( pNode->m_dWords[i].m_sWord.cstr()==NULL )
				pNode->m_dWords.Remove ( i-- );

		if ( pNode->m_dWords.GetLength()==0 )
		{
			m_dSpawned.RemoveValue ( pNode ); // OPTIMIZE!
			SafeDelete ( pNode );
			return NULL;
		}

		return pNode;
	}

	// sweep op node
	ARRAY_FOREACH ( i, pNode->m_dChildren )
	{
		pNode->m_dChildren[i] = SweepNulls ( pNode->m_dChildren[i] );
		if ( pNode->m_dChildren[i]==NULL )
			pNode->m_dChildren.Remove ( i-- );
	}

	if ( pNode->m_dChildren.GetLength()==0 )
	{
		m_dSpawned.RemoveValue ( pNode ); // OPTIMIZE!
		SafeDelete ( pNode );
		return NULL;
	}

	// remove redundancies if needed
	if ( pNode->GetOp()!=SPH_QUERY_NOT && pNode->m_dChildren.GetLength()==1 )
	{
		XQNode_t * pRet = pNode->m_dChildren[0];
		pNode->m_dChildren.Reset ();
		pRet->m_pParent = pNode->m_pParent;

		m_dSpawned.RemoveValue ( pNode ); // OPTIMIZE!
		SafeDelete ( pNode );
		return pRet;
	}

	// done
	return pNode;
}


bool XQParser_t::FixupNots ( XQNode_t * pNode )
{
	// no processing for plain nodes
	if ( !pNode || pNode->m_dWords.GetLength() )
		return true;

	// process 'em children
	ARRAY_FOREACH ( i, pNode->m_dChildren )
		if ( !FixupNots ( pNode->m_dChildren[i] ) )
			return false;

	// extract NOT subnodes
	CSphVector<XQNode_t*> dNots;
	ARRAY_FOREACH ( i, pNode->m_dChildren )
		if ( pNode->m_dChildren[i]->GetOp()==SPH_QUERY_NOT )
	{
		dNots.Add ( pNode->m_dChildren[i] );
		pNode->m_dChildren.RemoveFast ( i-- );
	}

	// no NOTs? we're square
	if ( !dNots.GetLength() )
		return true;

	// nothing but NOTs? we can't compute that
	if ( !pNode->m_dChildren.GetLength() )
	{
		m_pParsed->m_sParseError.SetSprintf ( "query is non-computable (node consists of NOT operators only)" );
		return false;
	}

	// NOT within OR? we can't compute that
	if ( pNode->GetOp()==SPH_QUERY_OR )
	{
		m_pParsed->m_sParseError.SetSprintf ( "query is non-computable (NOT is not allowed within OR)" );
		return false;
	}

	// NOT used in before operator
	if ( pNode->GetOp()==SPH_QUERY_BEFORE )
	{
		m_pParsed->m_sParseError.SetSprintf ( "query is non-computable (NOT cannot be used as before operand)" );
		return false;
	}

	// must be some NOTs within AND at this point, convert this node to ANDNOT
	assert ( pNode->GetOp()==SPH_QUERY_AND && pNode->m_dChildren.GetLength() && dNots.GetLength() );

	XQNode_t * pAnd = new XQNode_t ( pNode->m_dSpec );
	pAnd->SetOp ( SPH_QUERY_AND, pNode->m_dChildren );
	m_dSpawned.Add ( pAnd );

	XQNode_t * pNot = NULL;
	if ( dNots.GetLength()==1 )
	{
		pNot = dNots[0];
	} else
	{
		pNot = new XQNode_t ( pNode->m_dSpec );
		pNot->SetOp ( SPH_QUERY_OR, dNots );
		m_dSpawned.Add ( pNot );
	}

	pNode->SetOp ( SPH_QUERY_ANDNOT, pAnd, pNot );
	return true;
}


void XQParser_t::DeleteNodesWOFields ( XQNode_t * pNode )
{
	if ( !pNode )
		return;

	for ( int i = 0; i < pNode->m_dChildren.GetLength (); )
	{
		if ( pNode->m_dChildren[i]->m_dSpec.m_dFieldMask.TestAll ( false ) )
		{
			XQNode_t * pChild = pNode->m_dChildren[i];
			assert ( pChild->m_dChildren.GetLength()==0 );
			m_dSpawned.RemoveValue ( pChild );

			// this should be a leaf node
			SafeDelete ( pNode->m_dChildren[i] );
			pNode->m_dChildren.RemoveFast ( i );

		} else
		{
			DeleteNodesWOFields ( pNode->m_dChildren[i] );
			i++;
		}
	}
}


static bool CheckQuorumProximity ( XQNode_t * pNode, CSphString * pError )
{
	assert ( pError );
	if ( !pNode )
		return true;

	bool bQuorumPassed = ( pNode->GetOp()!=SPH_QUERY_QUORUM ||
		( pNode->m_iOpArg>0 && ( !pNode->m_bPercentOp || pNode->m_iOpArg<=100 ) ) );
	if ( !bQuorumPassed )
	{
		if ( pNode->m_bPercentOp )
			pError->SetSprintf ( "quorum threshold out of bounds 0.0 and 1.0f (%f)", 1.0f / 100.0f * pNode->m_iOpArg );
		else
			pError->SetSprintf ( "quorum threshold too low (%d)", pNode->m_iOpArg );
		return false;
	}

	if ( pNode->GetOp()==SPH_QUERY_PROXIMITY && pNode->m_iOpArg<1 )
	{
		pError->SetSprintf ( "proximity threshold too low (%d)", pNode->m_iOpArg );
		return false;
	}

	bool bValid = true;
	ARRAY_FOREACH_COND ( i, pNode->m_dChildren, bValid )
	{
		bValid &= CheckQuorumProximity ( pNode->m_dChildren[i], pError );
	}

	return bValid;
}


static void FixupDegenerates ( XQNode_t * pNode )
{
	if ( !pNode )
		return;

	if ( pNode->m_dWords.GetLength()==1 &&
		( pNode->GetOp()==SPH_QUERY_PHRASE || pNode->GetOp()==SPH_QUERY_PROXIMITY || pNode->GetOp()==SPH_QUERY_QUORUM ) )
	{
		pNode->SetOp ( SPH_QUERY_AND );
		return;
	}

	ARRAY_FOREACH ( i, pNode->m_dChildren )
		FixupDegenerates ( pNode->m_dChildren[i] );
}


bool XQParser_t::Parse ( XQQuery_t & tParsed, const char * sQuery, const ISphTokenizer * pTokenizer,
	const CSphSchema * pSchema, CSphDict * pDict, const CSphIndexSettings & tSettings )
{
	CSphScopedPtr<ISphTokenizer> pMyTokenizer ( pTokenizer->Clone ( true ) );
	pMyTokenizer->AddSpecials ( "()|-!@~\"/^$<" );
	pMyTokenizer->AddPlainChar ( '?' );
	pMyTokenizer->AddPlainChar ( '%' );

	// most outcomes are errors
	SafeDelete ( tParsed.m_pRoot );

	// check for relaxed syntax
	const char * OPTION_RELAXED = "@@relaxed";
	const int OPTION_RELAXED_LEN = strlen ( OPTION_RELAXED );

	m_bStopOnInvalid = true;
	if ( sQuery && strncmp ( sQuery, OPTION_RELAXED, OPTION_RELAXED_LEN )==0 && !sphIsAlpha ( sQuery[OPTION_RELAXED_LEN] ) )
	{
		sQuery += OPTION_RELAXED_LEN;
		m_bStopOnInvalid = false;
	}

	// setup parser
	m_pParsed = &tParsed;
	m_sQuery = (BYTE*) sQuery;
	m_iQueryLen = sQuery ? strlen(sQuery) : 0;
	m_pTokenizer = pMyTokenizer.Ptr();
	m_pSchema = pSchema;
	m_pDict = pDict;
	m_pCur = sQuery;
	m_iAtomPos = 0;
	m_iPendingNulls = 0;
	m_iPendingType = 0;
	m_pRoot = NULL;
	m_bEmpty = true;
	m_bEmptyStopword = ( tSettings.m_iStopwordStep==0 );
	m_iOvershortStep = tSettings.m_iOvershortStep;

	m_pTokenizer->SetBuffer ( m_sQuery, m_iQueryLen );
	int iRes = yyparse ( this );

	if ( ( iRes || !m_pParsed->m_sParseError.IsEmpty() ) && !m_bEmpty )
	{
		Cleanup ();
		return false;
	}

	DeleteNodesWOFields ( m_pRoot );
	m_pRoot = SweepNulls ( m_pRoot );
	FixupDegenerates ( m_pRoot );

	if ( !FixupNots ( m_pRoot ) )
	{
		Cleanup ();
		return false;
	}

	if ( !CheckQuorumProximity ( m_pRoot, &m_pParsed->m_sParseError ) )
	{
		Cleanup();
		return false;
	}

	if ( m_pRoot && m_pRoot->GetOp()==SPH_QUERY_NOT )
	{
		Cleanup ();
		m_pParsed->m_sParseError.SetSprintf ( "query is non-computable (single NOT operator)" );
		return false;
	}

	// all ok; might want to create a dummy node to indicate that
	m_dSpawned.Reset();
	tParsed.m_pRoot = m_pRoot ? m_pRoot : new XQNode_t ( *m_dStateSpec.Last() );
	return true;
}

//////////////////////////////////////////////////////////////////////////


#ifdef XQDEBUG
static void xqIndent ( int iIndent )
{
	iIndent *= 2;
	while ( iIndent-- )
		printf ( "|-" );
}

static void xqDump ( const XQNode_t * pNode, int iIndent )
{
#ifdef XQ_DUMP_NODE_ADDR
	printf ( "0x%08x ", pNode );
#endif
	if ( pNode->m_dChildren.GetLength() )
	{
		xqIndent ( iIndent );
		switch ( pNode->GetOp() )
		{
			case SPH_QUERY_AND: printf ( "AND:" ); break;
			case SPH_QUERY_OR: printf ( "OR:" ); break;
			case SPH_QUERY_NOT: printf ( "NOT:" ); break;
			case SPH_QUERY_ANDNOT: printf ( "ANDNOT:" ); break;
			case SPH_QUERY_BEFORE: printf ( "BEFORE:" ); break;
			case SPH_QUERY_PHRASE: printf ( "PHRASE:" ); break;
			case SPH_QUERY_PROXIMITY: printf ( "PROXIMITY:" ); break;
			case SPH_QUERY_QUORUM: printf ( "QUORUM:" ); break;
			case SPH_QUERY_NEAR: printf ( "NEAR:" ); break;
			case SPH_QUERY_SENTENCE: printf ( "SENTENCE:" ); break;
			case SPH_QUERY_PARAGRAPH: printf ( "PARAGRAPH:" ); break;
			default: printf ( "unknown-op-%d:", pNode->GetOp() ); break;
		}
		printf ( " (%d)\n", pNode->m_dChildren.GetLength() );
		ARRAY_FOREACH ( i, pNode->m_dChildren )
		{
			assert ( pNode->m_dChildren[i]->m_pParent==pNode );
			xqDump ( pNode->m_dChildren[i], iIndent+1 );
		}
	} else
	{
		xqIndent ( iIndent );
		printf ( "MATCH(%d,%d):", pNode->m_dSpec.m_dFieldMask.GetMask32(), pNode->m_iOpArg );

		ARRAY_FOREACH ( i, pNode->m_dWords )
		{
			const XQKeyword_t & tWord = pNode->m_dWords[i];

			const char * sLocTag = "";
			if ( tWord.m_bFieldStart ) sLocTag = ", start";
			if ( tWord.m_bFieldEnd ) sLocTag = ", end";

			printf ( " %s (qpos %d%s)", tWord.m_sWord.cstr(), tWord.m_iAtomPos, sLocTag );
		}
		printf ( "\n" );
	}
}
#endif


CSphString sphReconstructNode ( const XQNode_t * pNode, const CSphSchema * pSchema )
{
	CSphString sRes ( "" );

	if ( !pNode )
		return sRes;

	if ( pNode->m_dWords.GetLength() )
	{
		// say just words to me
		const CSphVector<XQKeyword_t> & dWords = pNode->m_dWords;
		ARRAY_FOREACH ( i, dWords )
			sRes.SetSprintf ( "%s %s", sRes.cstr(), dWords[i].m_sWord.cstr() );
		sRes.Trim ();

		switch ( pNode->GetOp() )
		{
		case SPH_QUERY_AND:			break;
		case SPH_QUERY_PHRASE:		sRes.SetSprintf ( "\"%s\"", sRes.cstr() ); break;
		case SPH_QUERY_PROXIMITY:	sRes.SetSprintf ( "\"%s\"~%d", sRes.cstr(), pNode->m_iOpArg ); break;
		case SPH_QUERY_QUORUM:		sRes.SetSprintf ( "\"%s\"/%d", sRes.cstr(), pNode->m_iOpArg ); break;
		case SPH_QUERY_NEAR:		sRes.SetSprintf ( "\"%s\"NEAR/%d", sRes.cstr(), pNode->m_iOpArg ); break;
		default:					assert ( 0 && "unexpected op in ReconstructNode()" ); break;
		}

		if ( !pNode->m_dSpec.m_dFieldMask.TestAll(true) )
		{
			CSphString sFields ( "" );
			for ( int i=0; i<CSphSmallBitvec::iTOTALBITS; i++ )
			{
				if ( !pNode->m_dSpec.m_dFieldMask.Test(i) )
					continue;

				if ( pSchema )
					sFields.SetSprintf ( "%s,%s", sFields.cstr(), pSchema->m_dFields[i].m_sName.cstr() );
				else
					sFields.SetSprintf ( "%s,%d", sFields.cstr(), pNode->m_dSpec.m_dFieldMask.GetMask32() );
			}

			sRes.SetSprintf ( "( @%s: %s )", sFields.cstr()+1, sRes.cstr() );
		} else
		{
			if ( pNode->GetOp()==SPH_QUERY_AND && dWords.GetLength()>1 )
				sRes.SetSprintf ( "( %s )", sRes.cstr() ); // wrap bag of words
		}

	} else
	{
		ARRAY_FOREACH ( i, pNode->m_dChildren )
		{
			if ( !i )
			{
				sRes = sphReconstructNode ( pNode->m_dChildren[i], pSchema );
			} else
			{
				const char * sOp = "(unknown-op)";
				switch ( pNode->GetOp() )
				{
				case SPH_QUERY_AND:		sOp = " "; break;
				case SPH_QUERY_OR:		sOp = "|"; break;
				case SPH_QUERY_NOT:		sOp = "NOT"; break;
				case SPH_QUERY_ANDNOT:	sOp = "AND NOT"; break;
				case SPH_QUERY_BEFORE:	sOp = "BEFORE"; break;
				case SPH_QUERY_NEAR:	sOp = "NEAR"; break;
				case SPH_QUERY_PHRASE:	sOp = ""; break;
				default:				assert ( 0 && "unexpected op in ReconstructNode()" ); break;
				}
				if ( pNode->GetOp()==SPH_QUERY_PHRASE )
					sRes.SetSprintf ( "\"%s %s\"", sRes.cstr(), sphReconstructNode ( pNode->m_dChildren[i], pSchema ).cstr() );
				else
					sRes.SetSprintf ( "%s %s %s", sRes.cstr(), sOp, sphReconstructNode ( pNode->m_dChildren[i], pSchema ).cstr() );
			}
		}

		if ( pNode->m_dChildren.GetLength()>1 )
			sRes.SetSprintf ( "( %s )", sRes.cstr() );
	}

	return sRes;
}


bool sphParseExtendedQuery ( XQQuery_t & tParsed, const char * sQuery, const ISphTokenizer * pTokenizer,
	const CSphSchema * pSchema, CSphDict * pDict, const CSphIndexSettings & tSettings )
{
	XQParser_t qp;
	bool bRes = qp.Parse ( tParsed, sQuery, pTokenizer, pSchema, pDict, tSettings );

#ifndef NDEBUG
	if ( bRes && tParsed.m_pRoot )
		tParsed.m_pRoot->Check ( true );
#endif

#ifdef XQDEBUG
	if ( bRes )
	{
		printf ( "\n--- query ---\n" );
		printf ( "%s\n", sQuery );
		xqDump ( tParsed.m_pRoot, 0 );
		printf ( "---\n" );
	}
#endif

	// moved here from ranker creation
	// as at that point term expansion could produce many terms from expanded term and this condition got failed
	tParsed.m_bSingleWord = ( tParsed.m_pRoot && tParsed.m_pRoot->m_dChildren.GetLength()==0 && tParsed.m_pRoot->m_dWords.GetLength()==1 );

	return bRes;
}

//////////////////////////////////////////////////////////////////////////
// COMMON SUBTREES DETECTION
//////////////////////////////////////////////////////////////////////////

/// Decides if given pTree is appropriate for caching or not. Currently we don't cache
/// the end values (leafs).
static bool IsAppropriate ( XQNode_t * pTree )
{
	if ( !pTree ) return false;

	// skip nodes that actually are leaves (eg. "AND smth" node instead of merely "smth")
	return !( pTree->m_dWords.GetLength()==1 && pTree->GetOp()!=SPH_QUERY_NOT );
}

typedef CSphOrderedHash < DWORD, uint64_t, IdentityHash_fn, 128 > CDwordHash;

// stores the pair of a tree, and the bitmask of common nodes
// which contains the tree.
class BitMask_t
{
	XQNode_t *		m_pTree;
	uint64_t		m_uMask;

public:
	BitMask_t ()
		: m_pTree ( NULL )
		, m_uMask ( 0ull )
	{}

	void Init ( XQNode_t * pTree, uint64_t uMask )
	{
		m_pTree = pTree;
		m_uMask = uMask;
	}

	inline uint64_t GetMask() const { return m_uMask; }
	inline XQNode_t * GetTree() const { return m_pTree; }
};

// a list of unique values.
class Associations_t : public CDwordHash
{
public:

	// returns true when add the second member.
	// The reason is that only one is not interesting for us,
	// but more than two will flood the caller.
	bool Associate2nd ( uint64_t uTree )
	{
		if ( Exists ( uTree ) )
			return false;
		Add ( 0, uTree );
		return GetLength()==2;
	}

	// merge with another similar
	void Merge ( const Associations_t& parents )
	{
		parents.IterateStart();
		while ( parents.IterateNext() )
			Associate2nd ( parents.IterateGetKey() );
	}
};

// associate set of nodes, common bitmask for these nodes,
// and gives the < to compare different pairs
class BitAssociation_t
{
private:
	const Associations_t *	m_pAssociations;
	mutable int				m_iBits;

	// The key method of subtree selection.
	// Most 'heavy' subtrees will be extracted first.
	inline int GetWeight() const
	{
		assert ( m_pAssociations );
		int iNodes = m_pAssociations->GetLength();
		if ( m_iBits==0 && m_uMask!=0 )
		{
			for ( uint64_t dMask = m_uMask; dMask; dMask >>=1 )
				m_iBits += (int)( dMask & 1 );
		}

		// current working formula is num_nodes^2 * num_hits
		return iNodes * iNodes * m_iBits;
	}

public:
	uint64_t			m_uMask;

	BitAssociation_t()
		: m_pAssociations ( NULL )
		, m_iBits ( 0 )
		, m_uMask ( 0 )
	{}

	void Init ( uint64_t uMask, const Associations_t* dNodes )
	{
		m_uMask = uMask;
		m_pAssociations = dNodes;
		m_iBits = 0;
	}

	bool operator< (const BitAssociation_t& second) const
	{
		return GetWeight() < second.GetWeight();
	}
};

// for pairs of values builds and stores the association "key -> list of values"
class CAssociations_t
	: public CSphOrderedHash < Associations_t, uint64_t, IdentityHash_fn, 128 >
{
	int		m_iBits;			// number of non-unique associations
public:

	CAssociations_t() : m_iBits ( 0 ) {}

	// Add the given pTree into the list of pTrees, associated with given uHash
	int Associate ( XQNode_t * pTree, uint64_t uHash )
	{
		if ( !Exists ( uHash ) )
			Add ( Associations_t(), uHash );
		if ( operator[]( uHash ).Associate2nd ( pTree->GetHash() ) )
			m_iBits++;
		return m_iBits;
	}

	// merge the existing association of uHash with given chain
	void MergeAssociations ( const Associations_t & chain, uint64_t uHash )
	{
		if ( !Exists ( uHash ) )
			Add ( chain, uHash );
		else
			operator[]( uHash ).Merge ( chain );
	}

	inline int GetBits() const { return m_iBits; }
};

// The main class for working with common subtrees
class RevealCommon_t : ISphNoncopyable
{
private:
	static const int			MAX_MULTINODES = 64;
	CSphVector<BitMask_t>		m_dBitmasks;		// all bitmasks for all the nodes
	CSphVector<uint64_t>		m_dSubQueries;		// final vector with roadmap for tree division.
	CAssociations_t				m_hNodes;			// initial accumulator for nodes
	CAssociations_t				m_hInterSections;	// initial accumulator for nodes
	CDwordHash					m_hBitOrders;		// order numbers for found common subnodes
	XQOperator_e				m_eOp;				// my operator which I process

private:

	// returns the order for given uHash (if any).
	inline int GetBitOrder ( uint64_t uHash ) const
	{
		if ( !m_hBitOrders.Exists ( uHash ) )
			return -1;
		return m_hBitOrders[uHash];
	}

	// recursively scans the whole tree and builds the maps
	// where a list of parents associated with every "leaf" nodes (i.e. with children)
	bool BuildAssociations ( XQNode_t * pTree )
	{
		if ( IsAppropriate ( pTree ) )
		{
			ARRAY_FOREACH ( i, pTree->m_dChildren )
			if ( ( !BuildAssociations ( pTree->m_dChildren[i] ) )
				|| ( ( m_eOp==pTree->GetOp() )
				&& ( m_hNodes.Associate ( pTree, pTree->m_dChildren[i]->GetHash() )>=MAX_MULTINODES ) ) )
			{
				return false;
			}
		}
		return true;
	}

	// Find all leafs, non-unique across the tree,
	// and associate the order number with every of them
	bool CalcCommonNodes ()
	{
		if ( !m_hNodes.GetBits() )
			return false; // there is totally no non-unique leaves
		int iBit = 0;
		m_hNodes.IterateStart();
		while ( m_hNodes.IterateNext() )
			if ( m_hNodes.IterateGet().GetLength() > 1 )
				m_hBitOrders.Add ( iBit++, m_hNodes.IterateGetKey() );
		assert ( m_hNodes.GetBits()==m_hBitOrders.GetLength() );
		m_hNodes.Reset(); ///< since from now we don't need this data anymore
		return true;
	}

	// recursively builds for every node the bitmaks
	// of common nodes it has as children
	void BuildBitmasks ( XQNode_t * pTree )
	{
		if ( !IsAppropriate ( pTree ) )
			return;

		if ( m_eOp==pTree->GetOp() )
		{
			// calculate the bitmask
			int iOrder;
			uint64_t dMask = 0;
			ARRAY_FOREACH ( i, pTree->m_dChildren )
			{
				iOrder = GetBitOrder ( pTree->m_dChildren[i]->GetHash() );
				if ( iOrder>=0 )
					dMask |= 1ull << iOrder;
			}

			// add the bitmask into the array
			if ( dMask )
				m_dBitmasks.Add().Init( pTree, dMask );
		}

		// recursively process all the children
		ARRAY_FOREACH ( i, pTree->m_dChildren )
			BuildBitmasks ( pTree->m_dChildren[i] );
	}

	// Collect all possible intersections of Bitmasks.
	// For every non-zero intersection we collect the list of trees which contain it.
	void CalcIntersections ()
	{
		// Round 1. Intersect all content of bitmasks one-by-one.
		ARRAY_FOREACH ( i, m_dBitmasks )
			for ( int j = i+1; j<m_dBitmasks.GetLength(); j++ )
			{
				// intersect one-by-one and group (grouping is done by nature of a hash)
				uint64_t uMask = m_dBitmasks[i].GetMask() & m_dBitmasks[j].GetMask();
				if ( uMask )
				{
					m_hInterSections.Associate ( m_dBitmasks[i].GetTree(), uMask );
					m_hInterSections.Associate ( m_dBitmasks[j].GetTree(), uMask );
				}
			}

		// Round 2. Intersect again all collected intersection one-by-one - until zero.
		void *p1=NULL, *p2;
		uint64_t uMask1, uMask2;
		while ( m_hInterSections.IterateNext ( &p1 ) )
		{
			p2 = p1;
			while ( m_hInterSections.IterateNext ( &p2 ) )
			{
				uMask1 = CAssociations_t::IterateGetKey ( &p1 );
				uMask2 = CAssociations_t::IterateGetKey ( &p2 );
				assert ( uMask1!=uMask2 );
				uMask1 &= uMask2;
				if ( uMask1 )
				{
					m_hInterSections.MergeAssociations ( CAssociations_t::IterateGet ( &p1 ), uMask1 );
					m_hInterSections.MergeAssociations ( CAssociations_t::IterateGet ( &p2 ), uMask1 );
				}
			}
		}
	}

	// create the final kit of common-subsets
	// which we will actually reveal (extract) from original trees
	void MakeQueries()
	{
		CSphVector<BitAssociation_t> dSubnodes; // masks for our selected subnodes
		dSubnodes.Reserve ( m_hInterSections.GetLength() );
		m_hInterSections.IterateStart();
		while ( m_hInterSections.IterateNext() )
			dSubnodes.Add().Init( m_hInterSections.IterateGetKey(), &m_hInterSections.IterateGet() );

		// sort by weight descending (weight sorting is hold by operator <)
		dSubnodes.RSort();
		m_dSubQueries.Reset();

		// make the final subtrees vector: get one-by-one from the beginning,
		// intresect with all the next and throw out zeros.
		// The final subqueries will not be intersected between each other.
		int j;
		uint64_t uMask;
		ARRAY_FOREACH ( i, dSubnodes )
		{
			uMask = dSubnodes[i].m_uMask;
			m_dSubQueries.Add ( uMask );
			j = i+1;
			while ( j < dSubnodes.GetLength() )
			{
				if ( !( dSubnodes[j].m_uMask &= ~uMask ) )
					dSubnodes.Remove(j);
				else
					j++;
			}
		}
	}

	// Now we finally extract the common subtrees from original tree
	// and (recursively) from it's children
	void Reorganize ( XQNode_t * pTree )
	{
		if ( !IsAppropriate ( pTree ) )
			return;

		if ( m_eOp==pTree->GetOp() )
		{
			// pBranch is for common subset of children, pOtherChildren is for the rest.
			CSphOrderedHash < XQNode_t*, int, IdentityHash_fn, 64 > hBranches;
			XQNode_t * pOtherChildren = NULL;
			int iBit;
			int iOptimizations = 0;
			ARRAY_FOREACH ( i, pTree->m_dChildren )
			{
				iBit = GetBitOrder ( pTree->m_dChildren[i]->GetHash() );

				// works only with children which are actually common with somebody else
				if ( iBit>=0 )
				{
					// since subqueries doesn't intersected between each other,
					// the first hit we found in this loop is exactly what we searched.
					ARRAY_FOREACH ( j, m_dSubQueries )
						if ( ( 1ull << iBit ) & m_dSubQueries[j] )
						{
							XQNode_t * pNode;
							if ( !hBranches.Exists(j) )
							{
								pNode = new XQNode_t ( pTree->m_dSpec );
								pNode->SetOp ( m_eOp, pTree->m_dChildren[i] );
								hBranches.Add ( pNode, j );
							} else
							{
								pNode = hBranches[j];
								pNode->m_dChildren.Add ( pTree->m_dChildren[i] );

								// Count essential subtrees (with at least 2 children)
								if ( pNode->m_dChildren.GetLength()==2 )
									iOptimizations++;
							}
							break;
						}
					// another nodes add to the set of "other" children
				} else
				{
					if ( !pOtherChildren )
					{
						pOtherChildren = new XQNode_t ( pTree->m_dSpec );
						pOtherChildren->SetOp ( m_eOp, pTree->m_dChildren[i] );
					} else
						pOtherChildren->m_dChildren.Add ( pTree->m_dChildren[i] );
				}
			}

			// we don't reorganize explicit simple case - as no "others" and only one common.
			// Also reject optimization if there is nothing to optimize.
			if ( ( iOptimizations==0 )
				| ( !pOtherChildren && ( hBranches.GetLength()==1 ) ) )
			{
				if ( pOtherChildren )
					pOtherChildren->m_dChildren.Reset();
				hBranches.IterateStart();
				while ( hBranches.IterateNext() )
				{
					assert ( hBranches.IterateGet() );
					hBranches.IterateGet()->m_dChildren.Reset();
					SafeDelete ( hBranches.IterateGet() );
				}
			} else
			{
				// reorganize the tree: replace the common subset to explicit node with
				// only common members inside. This will give the the possibility
				// to cache the node.
				pTree->m_dChildren.Reset();
				if ( pOtherChildren )
					pTree->m_dChildren.SwapData ( pOtherChildren->m_dChildren );

				hBranches.IterateStart();
				while ( hBranches.IterateNext() )
				{
					if ( hBranches.IterateGet()->m_dChildren.GetLength()==1 )
					{
						pTree->m_dChildren.Add ( hBranches.IterateGet()->m_dChildren[0] );
						hBranches.IterateGet()->m_dChildren.Reset();
						SafeDelete ( hBranches.IterateGet() );
					} else
						pTree->m_dChildren.Add ( hBranches.IterateGet() );
				}
			}
			SafeDelete ( pOtherChildren );
		}

		// recursively process all the children
		ARRAY_FOREACH ( i, pTree->m_dChildren )
			Reorganize ( pTree->m_dChildren[i] );
	}

public:
	explicit RevealCommon_t ( XQOperator_e eOp )
		: m_eOp ( eOp )
	{}

	// actual method for processing tree and reveal (extract) common subtrees
	void Transform ( int iXQ, const XQQuery_t * pXQ )
	{
		// collect all non-unique nodes
		for ( int i=0; i<iXQ; i++ )
			if ( !BuildAssociations ( pXQ[i].m_pRoot ) )
				return;

		// count and order all non-unique nodes
		if ( !CalcCommonNodes() )
			return;

		// create and collect bitmask for every node
		for ( int i=0; i<iXQ; i++ )
			BuildBitmasks ( pXQ[i].m_pRoot );

		// intersect all bitmasks one-by-one, and also intersect all intersections
		CalcIntersections();

		// the die-hard: actually select the set of subtrees which we'll process
		MakeQueries();

		// ... and finally - process all our trees.
		for ( int i=0; i<iXQ; i++ )
			Reorganize ( pXQ[i].m_pRoot );
	}
};


struct MarkedNode_t
{
	int			m_iCounter;
	XQNode_t *	m_pTree;
	bool		m_bMarked;
	int			m_iOrder;

	explicit MarkedNode_t ( XQNode_t * pTree=NULL )
		: m_iCounter ( 1 )
		, m_pTree ( pTree )
		, m_bMarked ( false )
		, m_iOrder ( 0 )
	{}

	void MarkIt ( bool bMark=true )
	{
		// mark
		if ( bMark )
		{
			m_iCounter++;
			m_bMarked = true;
			return;
		}

		// unmark
		if ( m_bMarked && m_iCounter>1 )
			m_iCounter--;
		if ( m_iCounter<2 )
			m_bMarked = false;
	}
};

typedef CSphOrderedHash < MarkedNode_t, uint64_t, IdentityHash_fn, 128 > CSubtreeHash;

/// check hashes, then check subtrees, then flag
static void FlagCommonSubtrees ( XQNode_t * pTree, CSubtreeHash & hSubTrees, bool bFlag=true, bool bMarkIt=true )
{
	if ( !IsAppropriate ( pTree ) )
		return;

	// we do not yet have any collisions stats,
	// but chances are we don't actually need IsEqualTo() at all
	uint64_t iHash = pTree->GetHash();
	if ( bFlag && hSubTrees.Exists ( iHash ) && hSubTrees [ iHash ].m_pTree->IsEqualTo ( pTree ) )
	{
		hSubTrees[iHash].MarkIt ();

		// we just add all the children but do NOT mark them as common
		// so that only the subtree root is marked.
		// also we unmark all the cases which were eaten by bigger trees
		ARRAY_FOREACH ( i, pTree->m_dChildren )
			if ( !hSubTrees.Exists ( pTree->m_dChildren[i]->GetHash() ) )
				FlagCommonSubtrees ( pTree->m_dChildren[i], hSubTrees, false, bMarkIt );
			else
				FlagCommonSubtrees ( pTree->m_dChildren[i], hSubTrees, false, false );
	} else
	{
		if ( !bMarkIt )
			hSubTrees[iHash].MarkIt(false);
		else
			hSubTrees.Add ( MarkedNode_t ( pTree ), iHash );

		ARRAY_FOREACH ( i, pTree->m_dChildren )
			FlagCommonSubtrees ( pTree->m_dChildren[i], hSubTrees, bFlag, bMarkIt );
	}
}


static void SignCommonSubtrees ( XQNode_t * pTree, CSubtreeHash & hSubTrees )
{
	if ( !pTree )
		return;

	uint64_t iHash = pTree->GetHash();
	if ( hSubTrees.Exists(iHash) && hSubTrees[iHash].m_bMarked )
		pTree->TagAsCommon ( hSubTrees[iHash].m_iOrder, hSubTrees[iHash].m_iCounter );

	ARRAY_FOREACH ( i, pTree->m_dChildren )
		SignCommonSubtrees ( pTree->m_dChildren[i], hSubTrees );
}


int sphMarkCommonSubtrees ( int iXQ, const XQQuery_t * pXQ )
{
	if ( iXQ<=0 || !pXQ )
		return 0;

	{ // Optional reorganize tree to extract common parts
		RevealCommon_t ( SPH_QUERY_AND ).Transform ( iXQ, pXQ );
		RevealCommon_t ( SPH_QUERY_OR ).Transform ( iXQ, pXQ );
	}

	// flag common subtrees and refcount them
	CSubtreeHash hSubtrees;
	for ( int i=0; i<iXQ; i++ )
		FlagCommonSubtrees ( pXQ[i].m_pRoot, hSubtrees );

	// number marked subtrees and assign them order numbers.
	int iOrder = 0;
	hSubtrees.IterateStart();
	while ( hSubtrees.IterateNext() )
		if ( hSubtrees.IterateGet().m_bMarked )
			hSubtrees.IterateGet().m_iOrder = iOrder++;

	// copy the flags and orders to original trees
	for ( int i=0; i<iXQ; i++ )
		SignCommonSubtrees ( pXQ[i].m_pRoot, hSubtrees );

	return iOrder;
}


// reset hash of tree nodes starting from pBottom node up to the root or node that has already reset hash
static void ResetHashUpTheTree ( XQNode_t * pBottom )
{
	if ( !pBottom || !pBottom->ResetHash() )
		return;

	ResetHashUpTheTree ( pBottom->m_pParent );
}


class CSphTransformation : public ISphNoncopyable
{
public:
	CSphTransformation ( XQNode_t ** ppRoot, const ISphKeywordsStat * pKeywords );
	void Transform ();
	inline void Dump ( const XQNode_t * pNode, const char * sHeader = "" );

private:

	typedef CSphOrderedHash < CSphVector<XQNode_t*>, uint64_t, IdentityHash_fn, 32> HashSimilar_t;
	CSphOrderedHash < HashSimilar_t, uint64_t, IdentityHash_fn, 256 >	m_hSimilar;
	CSphVector<XQNode_t *>		m_dRelatedNodes;
	const ISphKeywordsStat *	m_pKeywords;
	XQNode_t **					m_ppRoot;
	typedef bool ( *Checker_fn ) ( const XQNode_t * );

private:

	void		Dump ();
	void		SetCosts ( XQNode_t * pNode, const CSphVector<XQNode_t *> & dNodes );
	int			GetWeakestIndex ( const CSphVector<XQNode_t *> & dNodes );

	template < typename Group, typename SubGroup >
	inline void TreeCollectInfo ( XQNode_t * pParent, Checker_fn pfnChecker );

	template < typename Group, typename SubGroup >
	inline bool CollectInfo ( XQNode_t * pParent, Checker_fn pfnChecker );

	template < typename Excluder, typename Parenter >
	inline bool	CollectRelatedNodes ( const CSphVector<XQNode_t *> & dSimilarNodes );

	// ((A !N) | (B !N)) -> ((A|B) !N)
	static bool CheckCommonNot ( const XQNode_t * pNode );
	bool		TransformCommonNot ();
	bool		MakeTransformCommonNot ( CSphVector<XQNode_t *> & dSimilarNodes );

	// ((A !(N AA)) | (B !(N BB))) -> (((A|B) !N) | (A !AA) | (B !BB)) [ if cost(N) > cost(A) + cost(B) ]
	static bool	CheckCommonCompoundNot ( const XQNode_t * pNode );
	bool		TransformCommonCompoundNot ();
	bool		MakeTransformCommonCompoundNot ( CSphVector<XQNode_t *> & dSimilarNodes );

	// ((A (X | AA)) | (B (X | BB))) -> (((A|B) X) | (A AA) | (B BB)) [ if cost(X) > cost(A) + cost(B) ]
	static bool	CheckCommonSubTerm ( const XQNode_t * pNode );
	bool		TransformCommonSubTerm ();
	void		MakeTransformCommonSubTerm ( CSphVector<XQNode_t *> & dX );

	// (A | "A B"~N) -> A ; ("A B" | "A B C") -> "A B" ; ("A B"~N | "A B C"~N) -> ("A B"~N)
	static bool CheckCommonKeywords ( const XQNode_t * pNode );
	bool		TransformCommonKeywords ();

	// ("X A B" | "Y A B") -> (("X|Y") "A B")
	// ("A B X" | "A B Y") -> (("X|Y") "A B")
	static bool CheckCommonPhrase ( const XQNode_t * pNode );
	bool 		TransformCommonPhrase ();
	void		MakeTransformCommonPhrase ( CSphVector<XQNode_t *> & dCommonNodes, int iCommonLen, bool bHeadIsCommon );

	// ((A !X) | (A !Y) | (A !Z)) -> (A !(X Y Z))
	static bool CheckCommonAndNotFactor ( const XQNode_t * pNode );
	bool		TransformCommonAndNotFactor ();
	bool		MakeTransformCommonAndNotFactor ( CSphVector<XQNode_t *> & dSimilarNodes );

	// ((A !(N | N1)) | (B !(N | N2))) -> (( (A !N1) | (B !N2) ) !N)
	static bool CheckCommonOrNot ( const XQNode_t * pNode );
	bool 		TransformCommonOrNot ();
	bool		MakeTransformCommonOrNot ( CSphVector<XQNode_t *> & dSimilarNodes );

	// The main goal of transformations below is tree clarification and
	// further applying of standard transformations above.

	// "hung" operand ( AND(OR) node with only 1 child ) appears after an internal transformation
	static bool CheckHungOperand ( const XQNode_t * pNode );
	bool		TransformHungOperand ();

	// ((A | B) | C) -> ( A | B | C )
	// ((A B) C) -> ( A B C )
	static bool CheckExcessBrackets ( const XQNode_t * pNode );
	bool 		TransformExcessBrackets ();

	// ((A !N1) !N2) -> (A !(N1 | N2))
	static bool CheckExcessAndNot ( const XQNode_t * pNode );
	bool		TransformExcessAndNot ();

private:
	static const uint64_t CONST_GROUP_FACTOR;

	struct NullNode
	{
		static inline uint64_t By ( XQNode_t * ) { return CONST_GROUP_FACTOR; } // NOLINT
		static inline const XQNode_t * From ( const XQNode_t * ) { return NULL; } // NOLINT
	};

	struct CurrentNode
	{
		static inline uint64_t By ( XQNode_t * p ) { return p->GetFuzzyHash(); }
		static inline const XQNode_t * From ( const XQNode_t * p ) { return p; }
	};

	struct ParentNode
	{
		static inline uint64_t By ( XQNode_t * p ) { return p->m_pParent->GetFuzzyHash(); }
		static inline const XQNode_t * From ( const XQNode_t * p ) { return p->m_pParent; }
	};

	struct GrandNode
	{
		static inline uint64_t By ( XQNode_t * p ) { return p->m_pParent->m_pParent->GetFuzzyHash(); }
		static inline const XQNode_t * From ( const XQNode_t * p ) { return p->m_pParent->m_pParent; }
	};

	struct Grand2Node {
		static inline uint64_t By ( XQNode_t * p ) { return p->m_pParent->m_pParent->m_pParent->GetFuzzyHash(); }
		static inline const XQNode_t * From ( const XQNode_t * p ) { return p->m_pParent->m_pParent->m_pParent; }
	};

	struct Grand3Node
	{
		static inline uint64_t By ( XQNode_t * p ) { return p->m_pParent->m_pParent->m_pParent->m_pParent->GetFuzzyHash(); }
		static inline const XQNode_t * From ( const XQNode_t * p ) { return p->m_pParent->m_pParent->m_pParent->m_pParent; }
	};
};


CSphTransformation::CSphTransformation ( XQNode_t ** ppRoot, const ISphKeywordsStat * pKeywords )
	: m_pKeywords ( pKeywords )
	, m_ppRoot ( ppRoot )
{
	assert ( m_pKeywords!=NULL );
}



const uint64_t CSphTransformation::CONST_GROUP_FACTOR = 0;

template < typename Group, typename SubGroup >
void CSphTransformation::TreeCollectInfo ( XQNode_t * pParent, Checker_fn pfnChecker )
{
	if ( pParent )
	{
		if ( pfnChecker ( pParent ) )
		{
			// "Similar nodes" are nodes which are suited to a template (like 'COMMON NOT', 'COMMON COMPOND NOT', ...)
			uint64_t uGroup = (uint64_t)Group::From ( pParent );
			uint64_t uSubGroup = SubGroup::By ( pParent );

			HashSimilar_t & hGroup = m_hSimilar.AddUnique ( uGroup );
			hGroup.AddUnique ( uSubGroup ).Add ( pParent );
		}

		ARRAY_FOREACH ( iChild, pParent->m_dChildren )
			TreeCollectInfo<Group, SubGroup> ( pParent->m_dChildren[iChild], pfnChecker );
	}
}


template < typename Group, typename SubGroup >
bool CSphTransformation::CollectInfo ( XQNode_t * pParent, Checker_fn pfnChecker )
{
	( *m_ppRoot )->Check ( true );
	m_hSimilar.Reset();

	TreeCollectInfo<Group, SubGroup> ( pParent, pfnChecker );
	return ( m_hSimilar.GetLength()>0 );
}


void CSphTransformation::SetCosts ( XQNode_t * pNode, const CSphVector<XQNode_t *> & dNodes )
{
	assert ( pNode || dNodes.GetLength() );

	CSphVector<XQNode_t*> dChildren ( dNodes.GetLength() + 1 );
	dChildren[dNodes.GetLength()] = pNode;
	ARRAY_FOREACH ( i, dNodes )
	{
		dChildren[i] = dNodes[i];
		dChildren[i]->m_iUser = 0;
	}

	// collect unknown keywords from all children
	CSphVector<CSphKeywordInfo> dKeywords;
	SmallStringHash_T<int>	hCosts;
	ARRAY_FOREACH ( i, dChildren )
	{
		XQNode_t * pChild = dChildren[i];
		ARRAY_FOREACH ( j, pChild->m_dChildren )
		{
			dChildren.Add ( pChild->m_dChildren[j] );
			dChildren.Last()->m_iUser = 0;
			assert ( dChildren.Last()->m_pParent==pChild );
		}
		ARRAY_FOREACH ( j, pChild->m_dWords )
		{
			const CSphString & sWord = pChild->m_dWords[j].m_sWord;
			int * pCost = hCosts ( sWord );
			if ( !pCost )
			{
				Verify ( hCosts.Add ( 0, sWord ) );
				dKeywords.Add();
				dKeywords.Last().m_sTokenized = sWord;
				dKeywords.Last().m_iDocs = 0;
			}
		}
	}

	// get keywords info from index dictionary
	if ( dKeywords.GetLength() )
	{
		CSphString sError;
		m_pKeywords->FillKeywords ( dKeywords, sError );
		ARRAY_FOREACH ( i, dKeywords )
		{
			const CSphKeywordInfo & tKeyword = dKeywords[i];
			hCosts[tKeyword.m_sTokenized] = tKeyword.m_iDocs;
		}
	}

	// propagate cost bottom-up (from children to parents)
	for ( int i=dChildren.GetLength()-1; i>=0; i-- )
	{
		XQNode_t * pNode = dChildren[i];
		int iCost = 0;
		ARRAY_FOREACH ( j, pNode->m_dWords )
			iCost += hCosts [ pNode->m_dWords[j].m_sWord ];

		pNode->m_iUser += iCost;
		if ( pNode->m_pParent )
			pNode->m_pParent->m_iUser += pNode->m_iUser;
	}
}


template < typename Excluder, typename Parenter >
bool CSphTransformation::CollectRelatedNodes ( const CSphVector<XQNode_t *> & dSimilarNodes )
{
	m_dRelatedNodes.Resize ( 0 );
	ARRAY_FOREACH ( i, dSimilarNodes )
	{
		// Eval node that should be excluded
		const XQNode_t * pExclude = Excluder::From ( dSimilarNodes[i] );

		// Eval node that points to related nodes
		const XQNode_t * pParent = Parenter::From ( dSimilarNodes[i] );

		assert ( &pParent->m_dChildren!=&m_dRelatedNodes );
		ARRAY_FOREACH ( j, pParent->m_dChildren )
		{
			if ( pParent->m_dChildren[j]!=pExclude )
				m_dRelatedNodes.Add ( pParent->m_dChildren[j] );
		}
	}
	return ( m_dRelatedNodes.GetLength()>1 );
}


bool CSphTransformation::CheckCommonNot ( const XQNode_t * pNode )
{
	if ( !pNode || !pNode->m_pParent || !pNode->m_pParent->m_pParent || !pNode->m_pParent->m_pParent->m_pParent ||
			pNode->m_pParent->GetOp()!=SPH_QUERY_NOT || pNode->m_pParent->m_pParent->GetOp()!=SPH_QUERY_ANDNOT ||
			pNode->m_pParent->m_pParent->m_pParent->GetOp()!=SPH_QUERY_OR )
	{
//
// NOLINT		//  NOT:
// NOLINT		//		 _______ OR (gGOr) ___________
// NOLINT		// 		/          |                   |
// NOLINT		// 	 ...        AND NOT (grandAndNot)  ...
// NOLINT		//                 /       |
// NOLINT		//         relatedNode    NOT (parentNot)
// NOLINT		//         	               |
// NOLINT		//         	             pNode
//
		return false;
	}
	return true;
}


bool CSphTransformation::TransformCommonNot ()
{
	bool bRecollect = false;
	m_hSimilar.IterateStart();
	while ( m_hSimilar.IterateNext () )
	{
		m_hSimilar.IterateGet().IterateStart();
		while ( m_hSimilar.IterateGet().IterateNext() )
		{
			// Nodes with the same iFuzzyHash
			CSphVector<XQNode_t *> & dSimilarNodes = m_hSimilar.IterateGet().IterateGet();
			if ( dSimilarNodes.GetLength()<2 )
				continue;

			if ( CollectRelatedNodes < ParentNode, GrandNode > ( dSimilarNodes ) && MakeTransformCommonNot ( dSimilarNodes ) )
			{
				bRecollect = true;
				// Don't make transformation for other nodes from the same OR-node,
				// because query tree was changed and further transformations
				// might be invalid.
				break;
			}
		}
	}

	return bRecollect;
}


int CSphTransformation::GetWeakestIndex ( const CSphVector<XQNode_t *> & dNodes )
{
	// Returns index of weakest node from the equal.
	// The example of equal nodes:
	// "aaa bbb" (PHRASE), "aaa bbb"~10 (PROXIMITY), "aaa bbb"~20 (PROXIMITY)
	// Such nodes have the same magic hash value.
	// The weakest is "aaa bbb"~20

	int iWeakestIndex = 0;
	int iProximity = -1;

	ARRAY_FOREACH ( i, dNodes )
	{
		XQNode_t * pNode = dNodes[i];
		if ( pNode->GetOp()==SPH_QUERY_PROXIMITY && pNode->m_iOpArg>iProximity )
		{
			iProximity = pNode->m_iOpArg;
			iWeakestIndex = i;
		}
	}
	return iWeakestIndex;
}


bool CSphTransformation::MakeTransformCommonNot ( CSphVector<XQNode_t *> & dSimilarNodes )
{
	// Pick weakest node from the equal
	// PROXIMITY and PHRASE nodes with same keywords have an equal magic hash
	// so they are considered as equal nodes.
	int iWeakestIndex = GetWeakestIndex ( dSimilarNodes );

	// the weakest node is new parent of transformed expression
	XQNode_t * pWeakestAndNot = m_dRelatedNodes[iWeakestIndex]->m_pParent;
	assert ( pWeakestAndNot->m_dChildren[0]==m_dRelatedNodes[iWeakestIndex] );
	XQNode_t * pCommonOr = pWeakestAndNot->m_pParent;
	assert ( pCommonOr->GetOp()==SPH_QUERY_OR && pCommonOr->m_dChildren.Contains ( pWeakestAndNot ) );
	XQNode_t * pGrandCommonOr = pCommonOr->m_pParent;

	bool bKeepOr = ( pCommonOr->m_dChildren.GetLength()>2 );

	// reset ownership of related nodes
	ARRAY_FOREACH ( i, m_dRelatedNodes )
	{
		XQNode_t * pAnd = m_dRelatedNodes[i];
		XQNode_t * pAndNot = pAnd->m_pParent;
		assert ( pAndNot->m_pParent==pCommonOr );

		if ( i!=iWeakestIndex )
		{
			Verify ( pAndNot->m_dChildren.RemoveValue ( pAnd ) );

			if ( bKeepOr )
			{
				pCommonOr->m_dChildren.RemoveValue ( pAndNot );
				SafeDelete ( pAndNot );
			}
		}
	}

	// move all related to new OR
	XQNode_t * pHubOr = new XQNode_t ( XQLimitSpec_t() );
	pHubOr->SetOp ( SPH_QUERY_OR, m_dRelatedNodes );

	// insert hub OR via hub AND to new parent ( AND NOT )
	XQNode_t * pHubAnd = new XQNode_t ( XQLimitSpec_t() );
	pHubAnd->SetOp ( SPH_QUERY_AND, pHubOr );
	// replace old AND at new parent ( AND NOT ) 0 already at OR children
	pHubAnd->m_pParent = pWeakestAndNot;
	pWeakestAndNot->m_dChildren[0] = pHubAnd;

	// in case common OR had only 2 children
	if ( !bKeepOr )
	{
		// replace old OR with AND_NOT at parent
		if ( !pGrandCommonOr )
		{
			pWeakestAndNot->m_pParent = NULL;
			*m_ppRoot = pWeakestAndNot;
		} else
		{
			pWeakestAndNot->m_pParent = pGrandCommonOr;
			CSphVector<XQNode_t *> & dChildren = pGrandCommonOr->m_dChildren;
			ARRAY_FOREACH ( i, dChildren )
			{
				if ( dChildren[i]==pCommonOr )
				{
					dChildren[i] = pWeakestAndNot;
					break;
				}
			}
		}
		// remove new parent ( AND OR ) from OR children
		Verify ( pCommonOr->m_dChildren.RemoveValue ( pWeakestAndNot ) );
		// free OR and all children
		SafeDelete ( pCommonOr );
	}

	return true;
}


bool CSphTransformation::CheckCommonCompoundNot ( const XQNode_t * pNode )
{
	if ( !pNode || !pNode->m_pParent || !pNode->m_pParent->m_pParent || !pNode->m_pParent->m_pParent->m_pParent ||
			!pNode->m_pParent->m_pParent->m_pParent->m_pParent || pNode->m_pParent->GetOp()!=SPH_QUERY_AND ||
			pNode->m_pParent->m_pParent->GetOp()!=SPH_QUERY_NOT || pNode->m_pParent->m_pParent->m_pParent->GetOp()!=SPH_QUERY_ANDNOT ||
			pNode->m_pParent->m_pParent->m_pParent->m_pParent->GetOp()!=SPH_QUERY_OR )
	{
//
// NOLINT		//  NOT:
// NOLINT		//		 __ OR (Grand3 = CommonOr) __
// NOLINT		//		/    |                       |
// NOLINT		//	 ...  AND NOT (Grand2)          ...
// NOLINT		//          /        |
// NOLINT		//     relatedNode  NOT (grandNot)
// NOLINT		//        	         |
// NOLINT		//        	      AND (parentAnd)
// NOLINT		//                 /    |
// NOLINT		//               pNode  ...
//
		return false;
	}
	return true;
}


bool CSphTransformation::TransformCommonCompoundNot ()
{
	bool bRecollect = false;
	m_hSimilar.IterateStart();
	while ( m_hSimilar.IterateNext () )
	{
		m_hSimilar.IterateGet().IterateStart();
		while ( m_hSimilar.IterateGet().IterateNext() )
		{
			// Nodes with the same iFuzzyHash
			CSphVector<XQNode_t *> & dSimilarNodes = m_hSimilar.IterateGet().IterateGet();
			if ( dSimilarNodes.GetLength()<2 )
				continue;

			if ( CollectRelatedNodes < GrandNode, Grand2Node > ( dSimilarNodes ) )
			{
				// Load cost of the first node from the group
				// of the common nodes. The cost of nodes from
				// TransformableNodes are the same.
				SetCosts ( dSimilarNodes[0], m_dRelatedNodes );
				int iCommon = dSimilarNodes[0]->m_iUser;
				int iRelated = 0;
				ARRAY_FOREACH ( i, m_dRelatedNodes )
					iRelated += m_dRelatedNodes[i]->m_iUser;

				// Check that optimization willl be useful.
				if ( iCommon>iRelated && MakeTransformCommonCompoundNot ( dSimilarNodes ) )
				{
					bRecollect = true;
					// Don't make transformation for other nodes from the same OR-node,
					// because qtree was changed and further transformations
					// might be invalid.
					break;
				}
			}
		}
	}

	return bRecollect;
}


bool CSphTransformation::MakeTransformCommonCompoundNot ( CSphVector<XQNode_t *> & dSimilarNodes )
{
	// Pick weakest node from the equal
	// PROXIMITY and PHRASE nodes with same keywords have an equal magic hash
	// so they are considered as equal nodes.
	int iWeakestIndex = GetWeakestIndex ( dSimilarNodes );
	assert ( iWeakestIndex!=-1 );
	XQNode_t * pWeakestSimilar = dSimilarNodes [ iWeakestIndex ];

	// Common OR node (that is Grand3Node::From)
	XQNode_t * pCommonOr = pWeakestSimilar->m_pParent->m_pParent->m_pParent->m_pParent;

	// Factor out and delete/unlink similar nodes ( except weakest )
	ARRAY_FOREACH ( i, dSimilarNodes )
	{
		XQNode_t * pParent = dSimilarNodes[i]->m_pParent;
		Verify ( pParent->m_dChildren.RemoveValue ( dSimilarNodes[i] ) );

		if ( i!=iWeakestIndex )
			SafeDelete ( dSimilarNodes[i] );
	}

	// Create yet another ANDNOT node
	// with related nodes and one common node
	XQNode_t * pNewNot = new XQNode_t ( XQLimitSpec_t() );
	pNewNot->SetOp ( SPH_QUERY_NOT, pWeakestSimilar );

	XQNode_t * pNewOr = new XQNode_t ( XQLimitSpec_t() );
	pNewOr->SetOp ( SPH_QUERY_OR );
	pNewOr->m_dChildren.Resize ( m_dRelatedNodes.GetLength() );
	ARRAY_FOREACH ( i, m_dRelatedNodes )
	{
		// ANDNOT operation implies AND and NOT nodes.
		// The related nodes point to AND node that has one child node.
		assert ( m_dRelatedNodes[i]->m_dChildren.GetLength()==1 );
		pNewOr->m_dChildren[i] = m_dRelatedNodes[i]->m_dChildren[0]->Clone();
		pNewOr->m_dChildren[i]->m_pParent = pNewOr;
	}

	XQNode_t * pNewAnd = new XQNode_t ( XQLimitSpec_t() );
	pNewAnd->SetOp ( SPH_QUERY_AND, pNewOr );
	XQNode_t * pNewAndNot = new XQNode_t ( XQLimitSpec_t() );
	pNewAndNot->SetOp ( SPH_QUERY_ANDNOT, pNewAnd, pNewNot );
	pCommonOr->m_dChildren.Add ( pNewAndNot );
	pNewAndNot->m_pParent = pCommonOr;
	return true;
}


bool CSphTransformation::CheckCommonSubTerm ( const XQNode_t * pNode )
{
	if ( !pNode || ( pNode->GetOp()==SPH_QUERY_PHRASE && pNode->m_dChildren.GetLength() )
		|| !pNode->m_pParent || !pNode->m_pParent->m_pParent || !pNode->m_pParent->m_pParent->m_pParent ||
			pNode->m_pParent->GetOp()!=SPH_QUERY_OR || pNode->m_pParent->m_pParent->GetOp()!=SPH_QUERY_AND ||
			pNode->m_pParent->m_pParent->m_pParent->GetOp()!=SPH_QUERY_OR )
	{
//
// NOLINT		//  NOT:
// NOLINT		//        ________OR (gGOr)
// NOLINT		//		/           |
// NOLINT		//	......	  AND (grandAnd)
// NOLINT		//                /     |
// NOLINT		//      relatedNode    OR (parentOr)
// NOLINT		//        	            /   |
// NOLINT		//                   pNode  ...
//
		return false;
	}
	return true;
}


bool CSphTransformation::TransformCommonSubTerm ()
{
	bool bRecollect = false;
	m_hSimilar.IterateStart();
	while ( m_hSimilar.IterateNext () )
	{
		m_hSimilar.IterateGet().IterateStart();
		while ( m_hSimilar.IterateGet().IterateNext() )
		{
			// Nodes with the same iFuzzyHash
			CSphVector<XQNode_t *> & dX = m_hSimilar.IterateGet().IterateGet();
			if ( dX.GetLength()<2 )
				continue;

			// skip common sub-terms from same tree
			bool bSame = false;
			for ( int i=0; i<dX.GetLength()-1 && !bSame; i++ )
			{
				for ( int j=i+1; j<dX.GetLength() && !bSame; j++ )
					bSame = ( dX[i]->m_pParent==dX[j]->m_pParent );
			}
			if ( bSame )
				continue;

			if ( CollectRelatedNodes < ParentNode, GrandNode > ( dX ) )
			{
				// Load cost of the first node from the group
				// of the common nodes. The cost of nodes from
				// TransformableNodes are the same.
				SetCosts ( dX[0], m_dRelatedNodes );
				int iCostCommonSubTermNode = dX[0]->m_iUser;
				int iCostRelatedNodes = 0;
				ARRAY_FOREACH ( i, m_dRelatedNodes )
					iCostRelatedNodes += m_dRelatedNodes[i]->m_iUser;

				// Check that optimization will be useful.
				if ( iCostCommonSubTermNode > iCostRelatedNodes )
				{
					MakeTransformCommonSubTerm ( dX );
					bRecollect = true;
					// Don't make transformation for other nodes from the same OR-node,
					// because query tree was changed and further transformations
					// might be invalid.
					break;
				}
			}
		}
	}

	return bRecollect;
}


// remove nodes without children up the tree
static bool SubtreeRemoveEmpty ( XQNode_t * pNode )
{
	if ( !pNode->IsEmpty() )
		return false;

	// climb up
	XQNode_t * pParent = pNode->m_pParent;
	while ( pParent && pParent->m_dChildren.GetLength()<=1 && !pParent->m_dWords.GetLength() )
	{
		pNode = pParent;
		pParent = pParent->m_pParent;
	}

	if ( pParent )
		pParent->m_dChildren.RemoveValue ( pNode );

	// free subtree
	SafeDelete ( pNode );
	return true;
}


// eliminate composite ( AND \ OR ) nodes with only one children
static void CompositeFixup ( XQNode_t * pNode, XQNode_t ** ppRoot )
{
	assert ( pNode && !pNode->m_dWords.GetLength() );
	if ( pNode->m_dChildren.GetLength()!=1 || !( pNode->GetOp()==SPH_QUERY_OR || pNode->GetOp()==SPH_QUERY_AND ) )
		return;

	XQNode_t * pChild = pNode->m_dChildren[0];
	pChild->m_pParent = NULL;
	pNode->m_dChildren.Resize ( 0 );

	// climb up
	XQNode_t * pParent = pNode->m_pParent;
	while ( pParent && pParent->m_dChildren.GetLength()==1 && !pParent->m_dWords.GetLength() &&
		( pParent->GetOp()==SPH_QUERY_OR || pParent->GetOp()==SPH_QUERY_AND ) )
	{
		pNode = pParent;
		pParent = pParent->m_pParent;
	}

	if ( pParent )
	{
		ARRAY_FOREACH ( i, pParent->m_dChildren )
		{
			if ( pParent->m_dChildren[i]!=pNode )
				continue;

			pParent->m_dChildren[i] = pChild;
			pChild->m_pParent = pParent;
			break;
		}
	} else
	{
		*ppRoot = pChild;
	}

	// free subtree
	SafeDelete ( pNode );
}


static void CleanupSubtree ( XQNode_t * pNode, XQNode_t ** ppRoot )
{
	if ( SubtreeRemoveEmpty ( pNode ) )
		return;

	CompositeFixup ( pNode, ppRoot );
}


void CSphTransformation::MakeTransformCommonSubTerm ( CSphVector<XQNode_t *> & dX )
{
	// Pick weakest node from the equal
	// PROXIMITY and PHRASE nodes with same keywords have an equal magic hash
	// so they are considered as equal nodes.
	int iWeakestIndex = GetWeakestIndex ( dX );
	XQNode_t * pX = dX[iWeakestIndex];

	// common parents of X and AA \ BB need to be excluded
	CSphVector<XQNode_t *> dExcluded ( dX.GetLength() );

	// Factor out and delete/unlink similar nodes ( except weakest )
	ARRAY_FOREACH ( i, dX )
	{
		XQNode_t * pX = dX[i];
		XQNode_t * pParent = pX->m_pParent;
		Verify ( pParent->m_dChildren.RemoveValue ( pX ) );
		if ( i!=iWeakestIndex )
			SafeDelete ( dX[i] );

		dExcluded[i] = pParent;
		pParent->m_pParent->m_dChildren.RemoveValue ( pParent );
	}

	CSphVector<XQNode_t *> dRelatedParents;
	for ( int i=0; i<m_dRelatedNodes.GetLength(); i++ )
	{
		XQNode_t * pParent = m_dRelatedNodes[i]->m_pParent;
		if ( !dRelatedParents.Contains ( pParent ) )
			dRelatedParents.Add ( pParent );
	}

	ARRAY_FOREACH ( i, dRelatedParents )
		dRelatedParents[i] = dRelatedParents[i]->Clone();

	// push excluded children back
	ARRAY_FOREACH ( i, dExcluded )
	{
		XQNode_t * pChild = dExcluded[i];
		pChild->m_pParent->m_dChildren.Add ( pChild );
	}

	XQNode_t * pNewOr = new XQNode_t ( XQLimitSpec_t() );
	pNewOr->SetOp ( SPH_QUERY_OR, dRelatedParents );

	// Create yet another AND node
	// with related nodes and one common dSimilar node
	XQNode_t * pCommonOr = pX->m_pParent->m_pParent->m_pParent;
	XQNode_t * pNewAnd = new XQNode_t ( XQLimitSpec_t() );
	pNewAnd->SetOp ( SPH_QUERY_AND, pNewOr, pX );
	pCommonOr->m_dChildren.Add ( pNewAnd );
	pNewAnd->m_pParent = pCommonOr;

	ARRAY_FOREACH ( i, dExcluded )
	{
		CleanupSubtree ( dExcluded[i], m_ppRoot );
	}
}


bool CSphTransformation::CheckCommonKeywords ( const XQNode_t * pNode )
{
	if ( !pNode || !pNode->m_pParent || pNode->m_pParent->GetOp()!=SPH_QUERY_OR || !pNode->m_dWords.GetLength() )
	{
//
// NOLINT		//	NOT:
// NOLINT		// 		 ______________________ OR (parentOr) _______
// NOLINT		//		/                            |               |
// NOLINT		//	  pNode (PHRASE|AND|PROXIMITY)  ...	            ...
//
		return false;
	}
	return true;
}


typedef CSphOrderedHash<CSphVector<XQNode_t *>, uint64_t, IdentityHash_fn, 128> BigramHash_t;


static int sphBigramAddNode ( XQNode_t * pNode, int64_t uHash, BigramHash_t & hBirgam )
{
	CSphVector<XQNode_t *> * ppNodes = hBirgam ( uHash );
	if ( !ppNodes )
	{
		CSphVector<XQNode_t *> dNode ( 1 );
		dNode[0] = pNode;
		hBirgam.Add ( dNode, uHash );
		return 1;
	} else
	{
		(*ppNodes).Add ( pNode );
		return (*ppNodes).GetLength();
	}
}

static const BYTE g_sPhraseDelimiter[] = { 1 };

static uint64_t sphHashPhrase ( const XQNode_t * pNode )
{
	assert ( pNode );
	uint64_t uHash = SPH_FNV64_SEED;
	ARRAY_FOREACH ( i, pNode->m_dWords )
	{
		if ( i )
			uHash = sphFNV64 ( g_sPhraseDelimiter, sizeof(g_sPhraseDelimiter), uHash );
		uHash = sphFNV64cont ( (BYTE *)pNode->m_dWords[i].m_sWord.cstr(), uHash );
	}

	return uHash;
}


static void sphHashSubphrases ( XQNode_t * pNode, BigramHash_t & hBirgam )
{
	assert ( pNode );
	// skip whole phrase
	if ( pNode->m_dWords.GetLength()<=1 )
		return;

	const CSphVector<XQKeyword_t> & dWords = pNode->m_dWords;
	int iLen = dWords.GetLength();
	for ( int i=0; i<iLen; i++ )
	{
		uint64_t uSubPhrase = sphFNV64cont ( (BYTE *)dWords[i].m_sWord.cstr(), SPH_FNV64_SEED );
		sphBigramAddNode ( pNode, uSubPhrase, hBirgam );

		// skip whole phrase
		int iSubLen = ( i==0 ? iLen-1 : iLen );
		for ( int j=i+1; j<iSubLen; j++ )
		{
			uSubPhrase = sphFNV64 ( g_sPhraseDelimiter, sizeof(g_sPhraseDelimiter), uSubPhrase );
			uSubPhrase = sphFNV64cont ( (BYTE *)dWords[j].m_sWord.cstr(), uSubPhrase );
			sphBigramAddNode ( pNode, uSubPhrase, hBirgam );
		}
	}

	// loop all children
	ARRAY_FOREACH ( i, pNode->m_dChildren )
		sphHashSubphrases ( pNode->m_dChildren[i], hBirgam );
}


bool sphIsNodeStrongest ( const XQNode_t * pNode, const CSphVector<XQNode_t *> & dSimilar )
{
	//
	// The cases when query won't be optimized:
	// 1. Proximities with different distance: "A B C"~N | "A B C D"~M (N != M)
	// 2. Partial intersection in the middle: "A B C D" | "D B C E" (really they won't be found)
	// 3. Weaker phrase for proximity. Example: "A B C D"~N | "B C"
	//
	// The cases when query will be optimized:
	// 1. Found weaker term (phrase or proximity type) for sub-query with phrase type.
	// Examples:
	// "D A B C E" | "A B C" (weaker phrase) => "A B C"
	// "A B C D E" | "B C D"~N (weaker proximity) => "B C D"~N
	//
	// 2. Equal proximities with the different distance.
	// Example: "A B C"~N | "A B C"~M => "A B C"~min(M,N)
	//
	// 3. Found weaker term with proximity type with the same distance.
	// Example: "D A B C E"~N | "A B"~N => "A B"~N
	//

	assert ( pNode );
	XQOperator_e eNode = pNode->GetOp();
	int iWords = pNode->m_dWords.GetLength();

	ARRAY_FOREACH ( i, dSimilar )
	{
		XQOperator_e eSimilar = dSimilar[i]->GetOp();
		int iSimilarWords = dSimilar[i]->m_dWords.GetLength();

		if ( eNode==SPH_QUERY_PROXIMITY && eSimilar==SPH_QUERY_PROXIMITY && iWords>iSimilarWords )
			return false;
		if ( ( eNode==SPH_QUERY_PHRASE || eNode==SPH_QUERY_AND ) && ( eSimilar==SPH_QUERY_PROXIMITY && ( iWords>1 || pNode->m_dChildren.GetLength() ) ) )
			return false;

		bool bSimilar = ( ( eNode==SPH_QUERY_PHRASE && eSimilar==SPH_QUERY_PHRASE ) ||
			( ( eNode==SPH_QUERY_PHRASE || eNode==SPH_QUERY_AND ) && ( eSimilar==SPH_QUERY_PHRASE || eSimilar==SPH_QUERY_PROXIMITY ) ) ||
			( eNode==SPH_QUERY_PROXIMITY && ( eSimilar==SPH_QUERY_AND || eSimilar==SPH_QUERY_PHRASE ) ) ||
			( eNode==SPH_QUERY_PROXIMITY && eSimilar==SPH_QUERY_PROXIMITY && pNode->m_iOpArg>=dSimilar[i]->m_iOpArg ) );

		if ( !bSimilar )
			return false;
	}

	return true;
}


bool CSphTransformation::TransformCommonKeywords ()
{
	CSphVector <XQNode_t *> dPendingDel;
	m_hSimilar.IterateStart();
	while ( m_hSimilar.IterateNext () )
	{
		BigramHash_t hBigrams;
		m_hSimilar.IterateGet().IterateStart();
		while ( m_hSimilar.IterateGet().IterateNext() )
		{
			// Nodes with the same iFuzzyHash
			CSphVector<XQNode_t *> & dPhrases = m_hSimilar.IterateGet().IterateGet();
			if ( dPhrases.GetLength()<2 )
				continue;

			ARRAY_FOREACH ( i, dPhrases )
				sphHashSubphrases ( dPhrases[i], hBigrams );

			ARRAY_FOREACH ( i, dPhrases )
			{
				XQNode_t * pNode = dPhrases[i];
				uint64_t uPhraseHash = sphHashPhrase ( pNode );
				CSphVector<XQNode_t *> * ppCommon = hBigrams ( uPhraseHash );
				if ( ppCommon && sphIsNodeStrongest ( pNode, *ppCommon ) )
				{
					ARRAY_FOREACH ( j, (*ppCommon) )
						dPendingDel.Add ( (*ppCommon)[j] );
				}
			}
		}
	}

	bool bTransformed = ( dPendingDel.GetLength()>0 );
	dPendingDel.Sort();
	// Delete stronger terms
	XQNode_t * pLast = NULL;
	ARRAY_FOREACH ( i, dPendingDel )
	{
		// skip dupes
		if ( pLast==dPendingDel[i] )
			continue;

		pLast = dPendingDel[i];
		Verify ( pLast->m_pParent->m_dChildren.RemoveValue ( pLast ) );
		// delete here (not SafeDelete) as later that pointer will be compared
		delete ( dPendingDel[i] );
	}

	return bTransformed;
}

// minimum words per phrase that might be optimized by CommonSuffix optimization

bool CSphTransformation::CheckCommonPhrase ( const XQNode_t * pNode )
{
	if ( !pNode || !pNode->m_pParent || pNode->m_pParent->GetOp()!=SPH_QUERY_OR || pNode->GetOp()!=SPH_QUERY_PHRASE || pNode->m_dWords.GetLength()<2 )
	{
		//
		// NOLINT		//  NOT:
		// NOLINT		//		 ______________________ OR (parentOr) ___
		// NOLINT		// 		/                        |               |
		// NOLINT		// 	  pNode (PHRASE)            ...	             ...
		//
		return false;
	}

	// single word phrase not allowed
	assert ( pNode->m_dWords.GetLength()>=2 );

	// phrase there words not one after another not allowed
	for ( int i=1; i<pNode->m_dWords.GetLength(); i++ )
	{
		if ( pNode->m_dWords[i].m_iAtomPos-pNode->m_dWords[i-1].m_iAtomPos!=1 )
			return false;
	}

	return true;
}


struct CommonInfo_t
{
	CSphVector<XQNode_t *> *	m_pPhrases;
	int							m_iCommonLen;
	bool						m_bHead;
	bool						m_bHasBetter;
};


struct Node2Common_t
{
	XQNode_t * m_pNode;
	CommonInfo_t * m_pCommon;
};


struct CommonDupElemination_fn
{
	bool IsLess ( const Node2Common_t & a, const Node2Common_t & b ) const
	{
		if ( a.m_pNode!=b.m_pNode )
			return a.m_pNode<b.m_pNode;

		if ( a.m_pCommon->m_iCommonLen!=b.m_pCommon->m_iCommonLen )
			return a.m_pCommon->m_iCommonLen>b.m_pCommon->m_iCommonLen;

		return a.m_pCommon->m_bHead;
	}
};


struct XQNodeAtomPos_fn
{
	bool IsLess ( const XQNode_t * a, const XQNode_t * b ) const
	{
		return a->m_iAtomPos<b->m_iAtomPos;
	}
};


bool CSphTransformation::TransformCommonPhrase ()
{
	bool bRecollect = false;
	m_hSimilar.IterateStart();
	while ( m_hSimilar.IterateNext () )
	{
		m_hSimilar.IterateGet().IterateStart();
		while ( m_hSimilar.IterateGet().IterateNext() )
		{
			// Nodes with the same iFuzzyHash
			CSphVector<XQNode_t *> & dNodes = m_hSimilar.IterateGet().IterateGet();
			if ( dNodes.GetLength()<2 )
				continue;

			bool bHasCommonPhrases = false;
			BigramHash_t tBigramHead;
			BigramHash_t tBigramTail;
			// 1st check only 2 words at head tail at phrases
			ARRAY_FOREACH ( iPhrase, dNodes )
			{
				const CSphVector<XQKeyword_t> & dWords = dNodes[iPhrase]->m_dWords;
				assert ( dWords.GetLength()>=2 );
				dNodes[iPhrase]->m_iAtomPos = dWords.Begin()->m_iAtomPos;

				uint64_t uHead = sphFNV64cont ( (const BYTE *)dWords[0].m_sWord.cstr(), SPH_FNV64_SEED );
				uint64_t uTail = sphFNV64cont ( (const BYTE *)dWords [ dWords.GetLength() - 1 ].m_sWord.cstr(), SPH_FNV64_SEED );
				uHead = sphFNV64 ( g_sPhraseDelimiter, sizeof(g_sPhraseDelimiter), uHead );
				uHead = sphFNV64cont ( (const BYTE *)dWords[1].m_sWord.cstr(), uHead );

				uTail = sphFNV64 ( g_sPhraseDelimiter, sizeof(g_sPhraseDelimiter), uTail );
				uTail = sphFNV64cont ( (const BYTE *)dWords[dWords.GetLength()-2].m_sWord.cstr(), uTail );

				int iHeadLen = sphBigramAddNode ( dNodes[iPhrase], uHead, tBigramHead );
				int iTailLen = sphBigramAddNode ( dNodes[iPhrase], uTail, tBigramTail );
				bHasCommonPhrases |= ( iHeadLen>1 || iTailLen>1 );
			}

			if ( !bHasCommonPhrases )
				continue;

			// 2nd step find minimum for each phrases group
			CSphVector<CommonInfo_t> dCommon;
			tBigramHead.IterateStart();
			while ( tBigramHead.IterateNext() )
			{
				// only phrases that share same words at head
				if ( tBigramHead.IterateGet().GetLength()<2 )
					continue;

				CommonInfo_t & tElem = dCommon.Add();
				tElem.m_pPhrases = &tBigramHead.IterateGet();
				tElem.m_iCommonLen = 2;
				tElem.m_bHead = true;
				tElem.m_bHasBetter = false;
			}
			tBigramTail.IterateStart();
			while ( tBigramTail.IterateNext() )
			{
				// only phrases that share same words at tail
				if ( tBigramTail.IterateGet().GetLength()<2 )
					continue;

				CommonInfo_t & tElem = dCommon.Add();
				tElem.m_pPhrases = &tBigramTail.IterateGet();
				tElem.m_iCommonLen = 2;
				tElem.m_bHead = false;
				tElem.m_bHasBetter = false;
			}

			// for each set of phrases with common words at the head or tail
			// each word that is same at all phrases makes common length longer
			ARRAY_FOREACH ( i, dCommon )
			{
				CommonInfo_t & tCommon = dCommon[i];
				bool bHead = tCommon.m_bHead;
				const CSphVector<XQNode_t *> & dPhrases = *tCommon.m_pPhrases;
				// start from third word ( two words at each phrase already matched at bigram hashing )
				for ( int iCount=3; ; iCount++ )
				{
					// is shortest phrase words over
					if ( iCount>=dPhrases[0]->m_dWords.GetLength() )
						break;

					int iWordRef = ( bHead ? iCount-1 : dPhrases[0]->m_dWords.GetLength() - iCount );
					uint64_t uHash = sphFNV64 ( (const BYTE *)dPhrases[0]->m_dWords[iWordRef].m_sWord.cstr() );

					bool bPhrasesMatch = false;
					bool bSomePhraseOver = false;
					for ( int iPhrase=1; iPhrase<dPhrases.GetLength(); iPhrase++ )
					{
						bSomePhraseOver = ( iCount>=dPhrases[iPhrase]->m_dWords.GetLength() );
						if ( bSomePhraseOver )
							break;

						int iWord = ( bHead ? iCount-1 : dPhrases[iPhrase]->m_dWords.GetLength() - iCount );
						bPhrasesMatch = ( uHash==sphFNV64 ( (const BYTE *)dPhrases[iPhrase]->m_dWords[iWord].m_sWord.cstr() ) );
						if ( !bPhrasesMatch )
							break;
					}

					// no need to check further in case shortest phrase has no more words or sequence over
					if ( bSomePhraseOver || !bPhrasesMatch )
						break;

					tCommon.m_iCommonLen = iCount;
				}
			}

			// mark all dupes (that has smaller common length) as deleted
			if ( dCommon.GetLength()>=2 )
			{
				CSphVector<Node2Common_t> dDups ( dCommon.GetLength()*2 );
				dDups.Resize ( 0 );
				ARRAY_FOREACH ( i, dCommon )
				{
					CommonInfo_t & tCommon = dCommon[i];
					CSphVector<XQNode_t *> & dPhrases = *tCommon.m_pPhrases;
					ARRAY_FOREACH ( j, dPhrases )
					{
						Node2Common_t & tDup = dDups.Add();
						tDup.m_pNode = dPhrases[j];
						tDup.m_pCommon = &tCommon;
					}
				}
				dDups.Sort ( CommonDupElemination_fn() );
				for ( int i=0; i<dDups.GetLength()-1; i++ )
				{
					Node2Common_t & tCurr = dDups[i];
					Node2Common_t & tNext = dDups[i+1];
					if ( tCurr.m_pNode==tNext.m_pNode )
					{
						if ( tCurr.m_pCommon->m_iCommonLen<=tNext.m_pCommon->m_iCommonLen )
							tCurr.m_pCommon->m_bHasBetter = true;
						else
							tNext.m_pCommon->m_bHasBetter = true;
					}
				}
			}

			ARRAY_FOREACH ( i, dCommon )
			{
				const CommonInfo_t & tElem = dCommon[i];
				if ( !tElem.m_bHasBetter )
				{
					tElem.m_pPhrases->Sort ( XQNodeAtomPos_fn() );
					MakeTransformCommonPhrase ( *tElem.m_pPhrases, tElem.m_iCommonLen, tElem.m_bHead );
					bRecollect = true;
				}
			}
		}
	}

	return bRecollect;
}


void CSphTransformation::MakeTransformCommonPhrase ( CSphVector<XQNode_t *> & dCommonNodes, int iCommonLen, bool bHeadIsCommon )
{
	XQNode_t * pCommonPhrase = new XQNode_t ( XQLimitSpec_t() );
	pCommonPhrase->SetOp ( SPH_QUERY_PHRASE );

	XQNode_t * pGrandOr = dCommonNodes[0]->m_pParent;
	if ( bHeadIsCommon )
	{
		// fill up common suffix
		XQNode_t * pPhrase = dCommonNodes[0];
		pCommonPhrase->m_iAtomPos = pPhrase->m_dWords[0].m_iAtomPos;
		for ( int i=0; i<iCommonLen; i++ )
			pCommonPhrase->m_dWords.Add ( pPhrase->m_dWords[i] );
	} else
	{
		XQNode_t * pPhrase = dCommonNodes[0];
		// set the farthest atom position
		int iAtomPos = pPhrase->m_dWords [ pPhrase->m_dWords.GetLength() - iCommonLen ].m_iAtomPos;
		for ( int i=1; i<dCommonNodes.GetLength(); i++ )
		{
			XQNode_t * pCur = dCommonNodes[i];
			int iCurAtomPos = pCur->m_dWords[pCur->m_dWords.GetLength() - iCommonLen].m_iAtomPos;
			if ( iAtomPos < iCurAtomPos )
			{
				pPhrase = pCur;
				iAtomPos = iCurAtomPos;
			}
		}
		pCommonPhrase->m_iAtomPos = iAtomPos;

		for ( int i=pPhrase->m_dWords.GetLength() - iCommonLen; i<pPhrase->m_dWords.GetLength(); i++ )
			pCommonPhrase->m_dWords.Add ( pPhrase->m_dWords[i] );
	}

	XQNode_t * pNewOr = new XQNode_t ( XQLimitSpec_t() );
	pNewOr->SetOp ( SPH_QUERY_OR );

	ARRAY_FOREACH ( i, dCommonNodes )
	{
		XQNode_t * pPhrase = dCommonNodes[i];

		// remove phrase from parent and eliminate in case of common phrase duplication
		Verify ( pGrandOr->m_dChildren.RemoveValue ( pPhrase ) );
		if ( pPhrase->m_dWords.GetLength()==iCommonLen )
		{
			SafeDelete ( pPhrase );
			continue;
		}

		// move phrase to new OR
		pNewOr->m_dChildren.Add ( pPhrase );
		pPhrase->m_pParent = pNewOr;

		// shift down words and enumerate words atom positions
		if ( bHeadIsCommon )
		{
			int iEndCommonAtom = pCommonPhrase->m_dWords.Last().m_iAtomPos+1;
			for ( int i=iCommonLen; i<pPhrase->m_dWords.GetLength(); i++ )
			{
				int iTo = i-iCommonLen;
				pPhrase->m_dWords[iTo] = pPhrase->m_dWords[i];
				pPhrase->m_dWords[iTo].m_iAtomPos = iEndCommonAtom + iTo;
			}
		}
		pPhrase->m_dWords.Resize ( pPhrase->m_dWords.GetLength() - iCommonLen );
		if ( !bHeadIsCommon )
		{
			int iStartAtom = pCommonPhrase->m_dWords[0].m_iAtomPos - pPhrase->m_dWords.GetLength();
			ARRAY_FOREACH ( i, pPhrase->m_dWords )
				pPhrase->m_dWords[i].m_iAtomPos = iStartAtom + i;
		}

		if ( pPhrase->m_dWords.GetLength()==1 )
			pPhrase->SetOp ( SPH_QUERY_AND );
	}

	if ( pNewOr->m_dChildren.GetLength() )
	{
		// parent phrase need valid atom position of children
		pNewOr->m_iAtomPos = pNewOr->m_dChildren[0]->m_dWords[0].m_iAtomPos;

		XQNode_t * pNewPhrase = new XQNode_t ( XQLimitSpec_t() );
		if ( bHeadIsCommon )
			pNewPhrase->SetOp ( SPH_QUERY_PHRASE, pCommonPhrase, pNewOr );
		else
			pNewPhrase->SetOp ( SPH_QUERY_PHRASE, pNewOr, pCommonPhrase );

		pGrandOr->m_dChildren.Add ( pNewPhrase );
		pNewPhrase->m_pParent = pGrandOr;
	} else
	{
		// common phrases with same words elimination
		pGrandOr->m_dChildren.Add ( pCommonPhrase );
		pCommonPhrase->m_pParent = pGrandOr;
		SafeDelete ( pNewOr );
	}
}


bool CSphTransformation::CheckCommonAndNotFactor ( const XQNode_t * pNode )
{
	if ( !pNode || !pNode->m_pParent || !pNode->m_pParent->m_pParent || !pNode->m_pParent->m_pParent->m_pParent ||
			pNode->m_pParent->GetOp()!=SPH_QUERY_AND || pNode->m_pParent->m_pParent->GetOp()!=SPH_QUERY_ANDNOT ||
			pNode->m_pParent->m_pParent->m_pParent->GetOp()!=SPH_QUERY_OR ||
			// FIXME!!! check performance with OR node at 2nd grand instead of regular not NOT
			pNode->m_pParent->m_pParent->m_dChildren.GetLength()<2 || pNode->m_pParent->m_pParent->m_dChildren[1]->GetOp()!=SPH_QUERY_NOT )
	{
//
// NOLINT		//  NOT:
// NOLINT		//		 _______ OR (gGOr) ________________
// NOLINT		// 		/          |                       |
// NOLINT		// 	 ...        AND NOT (grandAndNot)     ...
// NOLINT		//                /       |
// NOLINT		//               AND     NOT
// NOLINT		//                |       |
// NOLINT		//              pNode  relatedNode
//
		return false;
	}
	return true;
}


bool CSphTransformation::TransformCommonAndNotFactor ()
{
	bool bRecollect = false;
	m_hSimilar.IterateStart();
	while ( m_hSimilar.IterateNext () )
	{
		m_hSimilar.IterateGet().IterateStart();
		while ( m_hSimilar.IterateGet().IterateNext() )
		{
			// Nodes with the same iFuzzyHash
			CSphVector<XQNode_t *> & dSimilarNodes = m_hSimilar.IterateGet().IterateGet();
			if ( dSimilarNodes.GetLength()<2 )
				continue;

			if ( MakeTransformCommonAndNotFactor ( dSimilarNodes ) )
				bRecollect = true;
		}
	}
	return bRecollect;
}


bool CSphTransformation::MakeTransformCommonAndNotFactor ( CSphVector<XQNode_t *> & dSimilarNodes )
{
	// Pick weakest node from the equal
	// PROXIMITY and PHRASE nodes with same keywords have an equal magic hash
	// so they are considered as equal nodes.
	int iWeakestIndex = GetWeakestIndex ( dSimilarNodes );

	XQNode_t * pFirstAndNot = dSimilarNodes [iWeakestIndex]->m_pParent->m_pParent;
	XQNode_t * pCommonOr = pFirstAndNot->m_pParent;

	assert ( pFirstAndNot->m_dChildren.GetLength()==2 );
	XQNode_t * pFirstNot = pFirstAndNot->m_dChildren[1];
	assert ( pFirstNot->m_dChildren.GetLength()==1 );

	XQNode_t * pAndNew = new XQNode_t ( XQLimitSpec_t() );
	pAndNew->SetOp ( SPH_QUERY_AND );
	pAndNew->m_dChildren.Reserve ( dSimilarNodes.GetLength() );
	pAndNew->m_dChildren.Add ( pFirstNot->m_dChildren[0] );
	pAndNew->m_dChildren.Last()->m_pParent = pAndNew;
	pFirstNot->m_dChildren[0] = pAndNew;
	pAndNew->m_pParent = pFirstNot;

	for ( int i=0; i<dSimilarNodes.GetLength(); ++i )
	{
		assert ( CheckCommonAndNotFactor ( dSimilarNodes[i] ) );
		if ( i==iWeakestIndex )
			continue;

		XQNode_t * pAndNot = dSimilarNodes[i]->m_pParent->m_pParent;
		assert ( pAndNot->m_dChildren.GetLength()==2 );
		XQNode_t * pNot = pAndNot->m_dChildren[1];
		assert ( pNot->m_dChildren.GetLength()==1 );
		assert ( &pAndNew->m_dChildren!=&pNot->m_dChildren );
		pAndNew->m_dChildren.Add ( pNot->m_dChildren[0] );
		pAndNew->m_dChildren.Last()->m_pParent = pAndNew;
		pNot->m_dChildren[0] = NULL;

		Verify ( pCommonOr->m_dChildren.RemoveValue ( pAndNot ) );
		dSimilarNodes[i] = NULL;
		SafeDelete ( pAndNot );
	}

	return true;
}


bool CSphTransformation::CheckCommonOrNot ( const XQNode_t * pNode )
{
	if ( !pNode || !pNode->m_pParent || !pNode->m_pParent->m_pParent || !pNode->m_pParent->m_pParent->m_pParent ||
			!pNode->m_pParent->m_pParent->m_pParent->m_pParent || pNode->m_pParent->GetOp()!=SPH_QUERY_OR ||
			pNode->m_pParent->m_pParent->GetOp()!=SPH_QUERY_NOT ||
			pNode->m_pParent->m_pParent->m_pParent->GetOp()!=SPH_QUERY_ANDNOT ||
			pNode->m_pParent->m_pParent->m_pParent->m_pParent->GetOp()!=SPH_QUERY_OR )
	{
//
// NOLINT		//  NOT:
// NOLINT		//		 __ OR (Grand3 = CommonOr) __
// NOLINT		//		/    |                       |
// NOLINT		//	 ...  AND NOT (Grand2)          ...
// NOLINT		//          /        |
// NOLINT		//     relatedNode  NOT (grandNot)
// NOLINT		//        	         |
// NOLINT		//        	      OR (parentOr)
// NOLINT		//                 /    |
// NOLINT		//               pNode  ...
//
		return false;
	}
	return true;
}


bool CSphTransformation::TransformCommonOrNot ()
{
	bool bRecollect = false;
	m_hSimilar.IterateStart();
	while ( m_hSimilar.IterateNext () )
	{
		m_hSimilar.IterateGet().IterateStart();
		while ( m_hSimilar.IterateGet().IterateNext() )
		{
			// Nodes with the same iFuzzyHash
			CSphVector<XQNode_t *> & dSimilarNodes = m_hSimilar.IterateGet().IterateGet();
			if ( dSimilarNodes.GetLength()<2 )
				continue;

			if ( CollectRelatedNodes < GrandNode, Grand2Node > ( dSimilarNodes ) && MakeTransformCommonOrNot ( dSimilarNodes ) )
			{
				bRecollect = true;
				// Don't make transformation for other nodes from the same OR-node,
				// because query tree was changed and further transformations
				// might be invalid.
				break;
			}
		}
	}

	return bRecollect;
}


bool CSphTransformation::MakeTransformCommonOrNot ( CSphVector<XQNode_t *> & dSimilarNodes )
{
	// Pick weakest node from the equal
	// PROXIMITY and PHRASE nodes with same keywords have an equal magic hash
	// so they are considered as equal nodes.
	int iWeakestIndex = GetWeakestIndex ( dSimilarNodes );
	assert ( iWeakestIndex!=-1 );
	XQNode_t * pWeakestSimilar = dSimilarNodes [ iWeakestIndex ];

	// Common OR node (that is Grand3Node::From)
	XQNode_t * pCommonOr = pWeakestSimilar->m_pParent->m_pParent->m_pParent->m_pParent;

	// Delete/unlink similar nodes ( except weakest )
	ARRAY_FOREACH ( i, dSimilarNodes )
	{
		XQNode_t * pParent = dSimilarNodes[i]->m_pParent;
		Verify ( pParent->m_dChildren.RemoveValue ( dSimilarNodes[i] ) );

		if ( i!=iWeakestIndex )
			SafeDelete ( dSimilarNodes[i] );
	}

	XQNode_t * pNewAndNot = new XQNode_t ( XQLimitSpec_t() );
	XQNode_t * pNewAnd = new XQNode_t ( XQLimitSpec_t() );
	XQNode_t * pNewNot = new XQNode_t ( XQLimitSpec_t() );
	if ( !pCommonOr->m_pParent )
	{
		*m_ppRoot = pNewAndNot;
	} else
	{
		pNewAndNot->m_pParent = pCommonOr->m_pParent;
		assert ( pCommonOr->m_pParent->m_dChildren.Contains ( pCommonOr ) );
		ARRAY_FOREACH ( i, pCommonOr->m_pParent->m_dChildren )
		{
			if ( pCommonOr->m_pParent->m_dChildren[i]==pCommonOr )
				pCommonOr->m_pParent->m_pParent->m_dChildren[i] = pNewAndNot;
		}
	}
	pNewAnd->SetOp ( SPH_QUERY_AND, pCommonOr );
	pNewNot->SetOp ( SPH_QUERY_NOT, pWeakestSimilar );
	pNewAndNot->SetOp ( SPH_QUERY_ANDNOT, pNewAnd, pNewNot );

	return true;
}


bool CSphTransformation::CheckHungOperand ( const XQNode_t * pNode )
{
	if ( !pNode || !pNode->m_pParent ||
			( pNode->m_pParent->GetOp()!=SPH_QUERY_OR && pNode->m_pParent->GetOp()!=SPH_QUERY_AND ) ||
			( pNode->m_pParent->m_pParent && pNode->m_pParent->GetOp()==SPH_QUERY_AND &&
				pNode->m_pParent->m_pParent->GetOp()==SPH_QUERY_ANDNOT ) ||
				pNode->m_pParent->m_dChildren.GetLength()>1 || pNode->m_dWords.GetLength() )
	{
//
// NOLINT		//  NOT:
// NOLINT		//	OR|AND (parent)
// NOLINT		//		  |
// NOLINT		//	    pNode\?
//
		return false;
	}
	return true;
}


bool CSphTransformation::TransformHungOperand ()
{
	if ( !m_hSimilar.GetLength() || !m_hSimilar.Exists ( CONST_GROUP_FACTOR ) || !m_hSimilar[CONST_GROUP_FACTOR].Exists ( CONST_GROUP_FACTOR ) )
		return false;

	CSphVector<XQNode_t *> & dSimilarNodes = m_hSimilar[CONST_GROUP_FACTOR][CONST_GROUP_FACTOR];
	ARRAY_FOREACH ( i, dSimilarNodes )
	{
		XQNode_t * pHungNode = dSimilarNodes[i];
		XQNode_t * pParent = pHungNode->m_pParent;
		XQNode_t * pGrand = pParent->m_pParent;

		if ( !pGrand )
		{
			*m_ppRoot = pHungNode;
			pHungNode->m_pParent = NULL;
		} else
		{
			assert ( pGrand->m_dChildren.Contains ( pParent ) );
			ARRAY_FOREACH ( i, pGrand->m_dChildren )
			{
				if ( pGrand->m_dChildren[i]!=pParent )
					continue;

				pGrand->m_dChildren[i] = pHungNode;
				pHungNode->m_pParent = pGrand;
				break;
			}
		}

		pParent->m_dChildren[0] = NULL;
		SafeDelete ( pParent );
	}

	return true;
}


bool CSphTransformation::CheckExcessBrackets ( const XQNode_t * pNode )
{
	if ( !pNode || !pNode->m_pParent || !pNode->m_pParent->m_pParent ||
			!( ( pNode->m_pParent->GetOp()==SPH_QUERY_AND && !pNode->m_pParent->m_dWords.GetLength() &&
					pNode->m_pParent->m_pParent->GetOp()==SPH_QUERY_AND ) ||
					( pNode->m_pParent->GetOp()==SPH_QUERY_OR && pNode->m_pParent->m_pParent->GetOp()==SPH_QUERY_OR ) ) )
	{
//
// NOLINT		//  NOT:
// NOLINT		//	         OR|AND (grand)
// NOLINT		//            /     |
// NOLINT		//	   OR|AND (parent) ...
// NOLINT		//		  |
// NOLINT		//	    pNode
//
		return false;
	}
	return true;
}

static XQNode_t * sphMoveSiblingsUp ( XQNode_t * pNode )
{
	XQNode_t * pParent = pNode->m_pParent;
	assert ( pParent );
	XQNode_t * pGrand = pParent->m_pParent;
	assert ( pGrand );

	assert ( pGrand->m_dChildren.Contains ( pParent ) );
	int iParent = GetNodeChildIndex ( pGrand, pParent );

	int iParentChildren = pParent->m_dChildren.GetLength();
	int iGrandChildren = pGrand->m_dChildren.GetLength();
	int iTotalChildren = iParentChildren+iGrandChildren-1;

	// parent.children + grand.parent.children - parent itself
	CSphVector<XQNode_t *> dChildren ( iTotalChildren );

	// grand head prior parent
	for ( int i=0; i<iParent; i++ )
		dChildren[i] = pGrand->m_dChildren[i];

	// grand tail after parent
	for ( int i=0; i<iGrandChildren-iParent-1; i++ )
		dChildren[i+iParent+iParentChildren] = pGrand->m_dChildren[i+iParent+1];

	// all parent children
	for ( int i=0; i<iParentChildren; i++ )
	{
		XQNode_t * pChild = pParent->m_dChildren[i];
		pChild->m_pParent = pGrand;
		dChildren[i+iParent] = pChild;
	}

	pGrand->m_dChildren.SwapData ( dChildren );
	// all children at grand now
	pParent->m_dChildren.Resize(0);
	delete ( pParent );
	return pParent;
}


struct XQNodeHash_fn
{
	static inline uint64_t Hash ( XQNode_t * pNode )	{ return (uint64_t)pNode; }
};


bool CSphTransformation::TransformExcessBrackets ()
{
	bool bRecollect = false;
	CSphOrderedHash<int, XQNode_t *, XQNodeHash_fn, 64> hDeleted;
	m_hSimilar.IterateStart();
	while ( m_hSimilar.IterateNext () )
	{
		m_hSimilar.IterateGet().IterateStart();
		while ( m_hSimilar.IterateGet().IterateNext() )
		{
			// Nodes with the same iFuzzyHash
			CSphVector<XQNode_t *> & dNodes = m_hSimilar.IterateGet().IterateGet();
			ARRAY_FOREACH ( i, dNodes )
			{
				XQNode_t * pNode = dNodes[i];
				// node environment might be changed due prior nodes transformations
				if ( !hDeleted.Exists ( pNode ) && CheckExcessBrackets ( pNode ) )
				{
					XQNode_t * pDel = sphMoveSiblingsUp ( pNode );
					hDeleted.Add ( 1, pDel );
					bRecollect = true;
				}
			}
		}
	}

	return bRecollect;
}


bool CSphTransformation::CheckExcessAndNot ( const XQNode_t * pNode )
{
	if ( !pNode || !ParentNode::From ( pNode ) || !GrandNode::From ( pNode ) || !Grand2Node::From ( pNode ) || pNode->GetOp()!=SPH_QUERY_AND ||
			( pNode->m_dChildren.GetLength()==1 && pNode->m_dChildren[0]->GetOp()==SPH_QUERY_ANDNOT ) ||
			ParentNode::From ( pNode )->GetOp()!=SPH_QUERY_ANDNOT || GrandNode::From ( pNode )->GetOp()!=SPH_QUERY_AND ||
			Grand2Node::From ( pNode )->GetOp()!=SPH_QUERY_ANDNOT ||
			// FIXME!!! check performance with OR node at 2nd grand instead of regular not NOT
			Grand2Node::From ( pNode )->m_dChildren.GetLength()<2 || Grand2Node::From ( pNode )->m_dChildren[1]->GetOp()!=SPH_QUERY_NOT )
	{
//
// NOLINT		//  NOT:
// NOLINT		//	                      AND NOT
// NOLINT		//	                       /   |
// NOLINT		//	                     AND  NOT
// NOLINT		//	                     | 
// NOLINT		//	                AND NOT
// NOLINT		//	              /      |
// NOLINT		//	         AND(pNode) NOT
// NOLINT		//            |          |
// NOLINT		//           ..         ...
//
		return false;
	}
	return true;
}


bool CSphTransformation::TransformExcessAndNot ()
{
	bool bRecollect = false;
	CSphOrderedHash<int, XQNode_t *, XQNodeHash_fn, 64> hDeleted;
	m_hSimilar.IterateStart();
	while ( m_hSimilar.IterateNext () )
	{
		m_hSimilar.IterateGet().IterateStart();
		while ( m_hSimilar.IterateGet().IterateNext() )
		{
			// Nodes with the same iFuzzyHash
			CSphVector<XQNode_t *> & dNodes = m_hSimilar.IterateGet().IterateGet();
			ARRAY_FOREACH ( i, dNodes )
			{
				XQNode_t * pAnd = dNodes[i];
				XQNode_t * pParentAndNot = pAnd->m_pParent;

				// node environment might be changed due prior nodes transformations
				if ( hDeleted.Exists ( pParentAndNot ) || !CheckExcessAndNot ( pAnd ) )
					continue;

				assert ( pParentAndNot->m_dChildren.GetLength()==2 );
				XQNode_t * pNot = pParentAndNot->m_dChildren[1];
				XQNode_t * pGrandAnd = pParentAndNot->m_pParent;
				XQNode_t * pGrand2AndNot = pGrandAnd->m_pParent;
				assert ( pGrand2AndNot->m_dChildren.GetLength()==2 );
				XQNode_t * pGrand2Not = pGrand2AndNot->m_dChildren[1];

				assert ( pGrand2Not->m_dChildren.GetLength()==1 );
				XQNode_t * pNewOr = new XQNode_t ( XQLimitSpec_t() );

				pNewOr->SetOp ( SPH_QUERY_OR, pNot->m_dChildren );
				pNewOr->m_dChildren.Add ( pGrand2Not->m_dChildren[0] );
				pNewOr->m_dChildren.Last()->m_pParent = pNewOr;
				pGrand2Not->m_dChildren[0] = pNewOr;
				pNewOr->m_pParent = pGrand2Not;

				assert ( pGrandAnd->m_dChildren.Contains ( pParentAndNot ) );
				int iChild = GetNodeChildIndex ( pGrandAnd, pParentAndNot );
				pGrandAnd->m_dChildren[iChild] = pAnd;
				pAnd->m_pParent = pGrandAnd;

				// Delete excess nodes
				hDeleted.Add ( 1, pParentAndNot );
				pNot->m_dChildren.Resize ( 0 );
				pParentAndNot->m_dChildren[0] = NULL;
				SafeDelete ( pParentAndNot );
				bRecollect = true;
			}
		}
	}

	return bRecollect;
}


void CSphTransformation::Dump ()
{
#ifdef XQDEBUG
	m_hSimilar.IterateStart();
	while ( m_hSimilar.IterateNext() )
	{
		printf ( "\nnode: hash 0x"UINT64_FMT"\n", m_hSimilar.IterateGetKey() );
		m_hSimilar.IterateGet().IterateStart();
		while ( m_hSimilar.IterateGet().IterateNext() )
		{
			CSphVector<XQNode_t *> & dNodes = m_hSimilar.IterateGet().IterateGet();
			printf ( "\tgrand: hash 0x"UINT64_FMT", children %d\n", m_hSimilar.IterateGet().IterateGetKey(), dNodes.GetLength() );

			printf ( "\tparents:\n" );
			ARRAY_FOREACH ( i, dNodes )
			{
				uint64_t uParentHash = dNodes[i]->GetHash();
				printf ( "\t\thash 0x"UINT64_FMT"\n", uParentHash );
			}
		}
	}
#endif
}


#ifdef XQDEBUG
void CSphTransformation::Dump ( const XQNode_t * pNode, const char * sHeader )
{
	printf ( sHeader );
	if ( pNode )
	{
		printf ( "%s\n", sphReconstructNode ( pNode, NULL ).cstr(), NULL );
#ifdef XQ_DUMP_TRANSFORMED_TREE
		xqDump ( pNode, 0 );
#endif
	}
}
#else
void CSphTransformation::Dump ( const XQNode_t * , const char * )
{}
#endif


void CSphTransformation::Transform ()
{
	if ( CollectInfo <ParentNode, NullNode> ( *m_ppRoot, &CheckCommonKeywords ) )
	{
		bool bDump = TransformCommonKeywords ();
		if ( bDump )
			Dump ( *m_ppRoot, "\nAfter  transformation of 'COMMON KEYWORDS'\n" );
	}

	if ( CollectInfo <ParentNode, NullNode> ( *m_ppRoot, &CheckCommonPhrase ) )
	{
		bool bDump = TransformCommonPhrase ();
		if ( bDump )
			Dump ( *m_ppRoot, "\nAfter  transformation of 'COMMON PHRASES'\n" );
	}

	bool bRecollect = false;
	do
	{
		bRecollect = false;

		if ( CollectInfo <Grand2Node, CurrentNode> ( *m_ppRoot, &CheckCommonNot ) )
		{
			bool bDump = TransformCommonNot ();
			bRecollect |= bDump;
			Dump ( bDump ? *m_ppRoot : NULL, "\nAfter  transformation of 'COMMON NOT'\n" );
		}

		if ( CollectInfo <Grand3Node, CurrentNode> ( *m_ppRoot, &CheckCommonCompoundNot ) )
		{
			bool bDump = TransformCommonCompoundNot ();
			bRecollect |= bDump;
			Dump ( bDump ? *m_ppRoot : NULL, "\nAfter  transformation of 'COMMON COMPOUND NOT'\n" );
		}

		if ( CollectInfo <Grand2Node, CurrentNode> ( *m_ppRoot, &CheckCommonSubTerm ) )
		{
			bool bDump = TransformCommonSubTerm ();
			bRecollect |= bDump;
			Dump ( bDump ? *m_ppRoot : NULL, "\nAfter  transformation of 'COMMON SUBTERM'\n" );
		}

		if ( CollectInfo <Grand2Node, CurrentNode> ( *m_ppRoot, &CheckCommonAndNotFactor ) )
		{
			bool bDump = TransformCommonAndNotFactor ();
			bRecollect |= bDump;
			Dump ( bDump ? *m_ppRoot : NULL, "\nAfter  transformation of 'COMMON ANDNOT FACTOR'\n" );
		}

		if ( CollectInfo <Grand3Node, CurrentNode> ( *m_ppRoot, &CheckCommonOrNot ) )
		{
			bool bDump = TransformCommonOrNot ();
			bRecollect |= bDump;
			Dump ( bDump ? *m_ppRoot : NULL, "\nAfter  transformation of 'COMMON OR NOT'\n" );
		}

		if ( CollectInfo <NullNode, NullNode> ( *m_ppRoot, &CheckHungOperand ) )
		{
			bool bDump = TransformHungOperand ();
			bRecollect |= bDump;
			Dump ( bDump ? *m_ppRoot : NULL, "\nAfter  transformation of 'HUNG OPERAND'\n" );
		}

		if ( CollectInfo <NullNode, NullNode> ( *m_ppRoot, &CheckExcessBrackets ) )
		{
			bool bDump = TransformExcessBrackets ();
			bRecollect |= bDump;
			Dump ( bDump ? *m_ppRoot : NULL, "\nAfter  transformation of 'EXCESS BRACKETS'\n" );
		}

		if ( CollectInfo <ParentNode, CurrentNode> ( *m_ppRoot, &CheckExcessAndNot ) )
		{
			bool bDump = TransformExcessAndNot ();
			bRecollect |= bDump;
			Dump ( bDump ? *m_ppRoot : NULL, "\nAfter  transformation of 'EXCESS AND NOT'\n" );
		}
	} while ( bRecollect );

	( *m_ppRoot )->Check ( true );
}


void sphOptimizeBoolean ( XQNode_t ** ppRoot, const ISphKeywordsStat * pKeywords )
{
#ifdef XQDEBUG
	int64_t tmDelta = sphMicroTimer();
#endif

	CSphTransformation qInfo ( ppRoot, pKeywords );
	qInfo.Transform ();

#ifdef XQDEBUG
	tmDelta = sphMicroTimer() - tmDelta;
	if ( tmDelta>10 )
		printf ( "optimized boolean in %d.%03d msec", (int)(tmDelta/1000), (int)(tmDelta%1000) );
#endif
}


//
// $Id$
//
