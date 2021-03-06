//============================================================================
// Copyright (C) 2014-2017 Intel Corporation.   All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice (including the next
// paragraph) shall be included in all copies or substantial portions of the
// Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//
// @file ${filename}
//
// @brief auto-generated file
//
// DO NOT EDIT
//
// Generation Command Line:
//  ${'\n//    '.join(cmdline)}
//
//============================================================================
#pragma once

//============================================================================
// Auto-generated ${comment}
//============================================================================
%for func in functions:
<%argList = ', '.join(func['args'])%>\
${func['decl']}
{
%if isX86:
    %if len(func['args']) != 0:
    SmallVector<Type*, ${len(func['args'])}> argTypes;
    %for arg in func['args']:
    argTypes.push_back(${arg}->getType());
    %endfor
    FunctionType* pFuncTy = FunctionType::get(${ func['returnType'] }, argTypes, false);
    %else:
    FunctionType* pFuncTy = FunctionType::get(${ func['returnType'] }, {}, false);
    %endif:
    Function* pFunc = cast<Function>(JM()->mpCurrentModule->getOrInsertFunction("meta.intrinsic.${func['name']}", pFuncTy));
    return CALL(pFunc, std::initializer_list<Value*>{${argList}}, name);
%elif isIntrin:
    %if len(func['types']) != 0:
    SmallVector<Type*, ${len(func['types'])}> args;
    %for arg in func['types']:
    args.push_back(${arg}->getType());
    %endfor
    Function * pFunc = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::${func['intrin']}, args);
    return CALL(pFunc, std::initializer_list<Value*>{${argList}}, name);
    %else:
    Function * pFunc = Intrinsic::getDeclaration(JM()->mpCurrentModule, Intrinsic::${func['intrin']});
    return CALL(pFunc, std::initializer_list<Value*>{${argList}}, name);
    %endif
%else:
    return IRB()->${func['intrin']}(${argList});
%endif
}

%endfor
