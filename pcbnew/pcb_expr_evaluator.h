/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2019-2020 KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */


#ifndef __PCB_EXPR_EVALUATOR_H
#define __PCB_EXPR_EVALUATOR_H

#include <unordered_map>

#include <property.h>
#include <property_mgr.h>

#include <libeval_compiler/libeval_compiler.h>


class BOARD_ITEM;

class PCB_EXPR_VAR_REF;

class PCB_EXPR_UCODE final : public LIBEVAL::UCODE
{
public:
    PCB_EXPR_UCODE() {};
    virtual ~PCB_EXPR_UCODE() {};

    virtual LIBEVAL::VAR_REF* createVarRef( LIBEVAL::COMPILER *aCompiler, const char* aVar,
                                            const char* aField ) override;

    virtual FUNC_PTR createFuncCall( LIBEVAL::COMPILER* aCompiler, const char* aName ) override;
};


class PCB_EXPR_CONTEXT : public LIBEVAL::CONTEXT
{
public:
    PCB_EXPR_CONTEXT()
    {
        m_items[0] = nullptr;
        m_items[1] = nullptr;
    }

    void SetItems( BOARD_ITEM* a, BOARD_ITEM* b = nullptr )
    {
        m_items[0] = a;
        m_items[1] = b;
    }

    BOARD_ITEM* GetItem( int index ) const
    {
        return m_items[index];
    }

private:
    BOARD_ITEM* m_items[2];
};


class PCB_EXPR_VAR_REF : public LIBEVAL::VAR_REF
{
public:
    PCB_EXPR_VAR_REF( int aItemIndex ) : 
        m_itemIndex( aItemIndex ),
        m_type( LIBEVAL::VT_UNDEFINED ),
        m_isEnum( false )
    {
        //printf("*** createVarRef %p %d\n", this, aItemIndex );
    }

    void SetIsEnum( bool s ) { m_isEnum = s; }
    bool IsEnum() const { return m_isEnum; }

    void SetType( LIBEVAL::VAR_TYPE_T type ) { m_type = type; }
    LIBEVAL::VAR_TYPE_T GetType() override { return m_type; }

    void AddAllowedClass( TYPE_ID type_hash, PROPERTY_BASE* prop )
    {
        m_matchingTypes[type_hash] = prop;
    }

    virtual LIBEVAL::VALUE GetValue( LIBEVAL::CONTEXT* aCtx ) override;

    BOARD_ITEM* GetObject( LIBEVAL::CONTEXT* aCtx ) const;

private:
    std::unordered_map<TYPE_ID, PROPERTY_BASE*> m_matchingTypes;
    int                                         m_itemIndex;
    LIBEVAL::VAR_TYPE_T                         m_type;
    bool                                        m_isEnum;
};


class PCB_EXPR_BUILTIN_FUNCTIONS
{
public:
    using FPTR = LIBEVAL::UCODE::FUNC_PTR;

    PCB_EXPR_BUILTIN_FUNCTIONS();

    static PCB_EXPR_BUILTIN_FUNCTIONS& Instance()
    {
        static PCB_EXPR_BUILTIN_FUNCTIONS self;
        return self;
    }

    LIBEVAL::UCODE::FUNC_PTR Get( const std::string &name )
    {
        return m_funcs[ name  ];
    }

    const wxArrayString GetSignatures() const
    {
        return m_funcSigs;
    }

private:
    std::map<std::string, LIBEVAL::UCODE::FUNC_PTR> m_funcs;

    wxArrayString m_funcSigs;
};


class PCB_EXPR_COMPILER : public LIBEVAL::COMPILER
{
public:
    PCB_EXPR_COMPILER();
};


class PCB_EXPR_EVALUATOR
{
public:
    PCB_EXPR_EVALUATOR();
    ~PCB_EXPR_EVALUATOR();

    bool Evaluate( const wxString& aExpr );
    int  Result() const { return m_result; }

    LIBEVAL::ERROR_STATUS GetErrorStatus() { return m_errorStatus; }

private:
    int  m_result;

    PCB_EXPR_COMPILER     m_compiler;
    PCB_EXPR_UCODE        m_ucode;

    LIBEVAL::ERROR_STATUS m_errorStatus;
};

#endif
