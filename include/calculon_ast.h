/* Calculon © 2013 David Given
 * This code is made available under the terms of the Simplified BSD License.
 * Please see the COPYING file for the full license text.
 */

#ifndef CALCULON_AST_H
#define CALCULON_AST_H

#ifndef CALCULON_H
#error "Don't include this, include calculon.h instead."
#endif

class ASTFrame;

struct ASTNode : public Object
{
	ASTNode* parent;
	Position position;

	ASTNode(const Position& position):
		parent(NULL),
		position(position)
	{
	}

	virtual ~ASTNode()
	{
	}

	virtual llvm::Value* codegen(Compiler& compiler) = 0;

	llvm::Value* codegen_to_type(Compiler& compiler, Type* type)
	{
		llvm::Value* v = codegen(compiler);
		Type* t = compiler.types->find(v->getType());

		if (!t->equals(type))
		{
			std::stringstream s;
			s << "type mismatch: expected a " << type->name << ", but got a "
			  << t->name;
			throw TypeException(s.str(), this);
		}
		return v;
	}

	llvm::Value* codegen_to_real(Compiler& compiler)
	{
		return codegen_to_type(compiler, compiler.realType);
	}

	llvm::Value* codegen_to_boolean(Compiler& compiler)
	{
		return codegen_to_type(compiler, compiler.booleanType);
	}

	virtual void resolveVariables(Compiler& compiler)
	{
	}

	virtual ASTFrame* getFrame()
	{
		return parent->getFrame();
	}

	virtual FunctionSymbol* getFunction()
	{
		return parent->getFunction();
	}
};

struct ASTConstant : public ASTNode
{
	Real value;

	ASTConstant(const Position& position, Real value):
		ASTNode(position),
		value(value)
	{
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		return llvm::ConstantFP::get(compiler.realType->llvm, value);
	}
};

struct ASTBoolean : public ASTNode
{
	string id;

	ASTBoolean(const Position& position, const string& id):
		ASTNode(position),
		id(id)
	{
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		if (id == "true")
			return llvm::ConstantInt::getTrue(compiler.booleanType->llvm);
		else
			return llvm::ConstantInt::getFalse(compiler.booleanType->llvm);
	}
};

struct ASTVariable : public ASTNode
{
	string id;
	ValuedSymbol* symbol;

	using ASTNode::getFrame;
	using ASTNode::getFunction;
	using ASTNode::position;

	ASTVariable(const Position& position, const string& id):
		ASTNode(position),
		id(id), symbol(NULL)
	{
	}

	void resolveVariables(Compiler& compiler)
	{
		SymbolTable* symbolTable = getFrame()->symbolTable;
		Symbol* s = symbolTable->resolve(id);
		if (!s)
			throw SymbolException(id, this);
		symbol = s->isValued();
		if (!symbol)
		{
			std::stringstream s;
			s << "attempt to get the value of '" << id << "', which is not a variable";
			throw CompilationException(position.formatError(s.str()));
		}

		VariableSymbol* v = symbol->isVariable();
		if (v)
			symbol = getFunction()->importUpvalue(compiler, v);
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		return symbol->emitValue(compiler);
	}
};

struct ASTVector : public ASTNode
{
	vector<ASTNode*> elements;
	string typenm;

	ASTVector(const Position& position, const vector<ASTNode*>& elements):
		ASTNode(position),
		elements(elements)
	{
		for (unsigned i = 0; i < elements.size(); i++)
			elements[i]->parent = this;

		std::stringstream s;
		s << "vector*" << elements.size();
		typenm = s.str();
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		VectorType* type = compiler.types->find(typenm)->asVector();
		llvm::Value* v = llvm::UndefValue::get(type->llvm);

		for (unsigned i = 0; i < elements.size(); i++)
		{
			llvm::Value* e = elements[i]->codegen_to_real(compiler);
			v = type->setElement(v, i, e);
		}

		return v;
	}

	void resolveVariables(Compiler& compiler)
	{
		for (unsigned i = 0; i < elements.size(); i++)
			elements[i]->resolveVariables(compiler);
	}
};

struct ASTVectorSplat : public ASTNode
{
	ASTNode* value;
	unsigned size;
	string typenm;

	ASTVectorSplat(const Position& position, ASTNode* value, unsigned size):
		ASTNode(position),
		value(value),
		size(size)
	{
		value->parent = this;

		std::stringstream s;
		s << "vector*" << size;
		typenm = s.str();
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		VectorType* type = compiler.types->find(typenm)->asVector();
		llvm::Value* v = llvm::UndefValue::get(type->llvm);

		llvm::Value* e = value->codegen_to_real(compiler);
		for (unsigned i = 0; i < size; i++)
			v = type->setElement(v, i, e);

		return v;
	}

	void resolveVariables(Compiler& compiler)
	{
		value->resolveVariables(compiler);
	}
};

struct ASTFrame : public ASTNode
{
	SymbolTable* symbolTable;

	ASTFrame(const Position& position):
		ASTNode(position),
		symbolTable(NULL)
	{
	}

	ASTFrame* getFrame()
	{
		return this;
	}
};

struct ASTDefineVariable : public ASTFrame
{
	string id;
	Type* type;
	ASTNode* value;
	ASTNode* body;

	VariableSymbol* _symbol;
	using ASTFrame::symbolTable;
	using ASTNode::parent;
	using ASTNode::getFunction;

	ASTDefineVariable(const Position& position,
			const string& id, Type* type,
			ASTNode* value, ASTNode* body):
		ASTFrame(position),
		id(id), type(type), value(value), body(body),
		_symbol(NULL)
	{
		body->parent = this;
	}

	void resolveVariables(Compiler& compiler)
	{
		symbolTable = compiler.retain(new SingletonSymbolTable(
				parent->getFrame()->symbolTable));
		_symbol = compiler.retain(new VariableSymbol(id, type));
		_symbol->function = getFunction();
		symbolTable->add(_symbol);

		_symbol->function->locals[_symbol] = _symbol;

		value->parent = parent;
		value->resolveVariables(compiler);
		body->resolveVariables(compiler);
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		llvm::Value* v = value->codegen(compiler);
		_symbol->value = v;

		if (!v)
		{
			std::stringstream s;
			s << "you can't assign 'return' to anything";
			throw TypeException(s.str(), this);
		}

		if (!type)
		{
			/* Now we have a value for this variable, we can find out what
			 * type it is.
			 */

			_symbol->type = type = compiler.types->find(v->getType());
		}

		if (v->getType() != type->llvm)
		{
			std::stringstream s;
			s << "variable is declared to return a "
			  << type->name
			  << " but has been set to a "
			  << compiler.types->find(v->getType())->name;

			throw TypeException(s.str(), this);
		}

		return body->codegen(compiler);
	}
};

struct ASTFunctionBody : public ASTFrame
{
	FunctionSymbol* function;
	ASTNode* body;

	using ASTNode::parent;
	using ASTFrame::symbolTable;

	ASTFunctionBody(const Position& position,
			FunctionSymbol* function, ASTNode* body):
		ASTFrame(position),
		function(function), body(body)
	{
		body->parent = this;
	}

	FunctionSymbol* getFunction()
	{
		return function;
	}

	void resolveVariables(Compiler& compiler)
	{
		if (!symbolTable)
			symbolTable = compiler.retain(new MultipleSymbolTable(
				parent->getFrame()->symbolTable));

		const vector<VariableSymbol*>& arguments = function->arguments;
		for (typename vector<VariableSymbol*>::const_iterator i = arguments.begin(),
				e = arguments.end(); i != e; i++)
		{
			VariableSymbol* symbol = *i;
			symbol->function = function;
			symbolTable->add(symbol);
		}

		body->resolveVariables(compiler);
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		/* Assemble the LLVM function type. */

		const vector<VariableSymbol*>& arguments = function->arguments;
		vector<llvm::Type*> llvmtypes;

		/* Normal parameters... */

		for (typename vector<VariableSymbol*>::const_iterator i = arguments.begin(),
				e = arguments.end(); i != e; i++)
		{
			llvmtypes.push_back((*i)->type->llvm);
		}

		/* ...and imported upvalues. */

		for (typename FunctionSymbol::LocalsMap::const_iterator i = function->locals.begin(),
				e = function->locals.end(); i != e; i++)
		{
			if (i->first != i->second)
			{
				VariableSymbol* symbol = i->first; // root variable!
				assert(symbol->value);
				llvmtypes.push_back(symbol->value->getType());
			}
		}

		llvm::Type* returntype = function->returntype->llvm;
		llvm::FunctionType* ft = llvm::FunctionType::get(
				returntype, llvmtypes, false);

		/* Create the function. */

		llvm::Function* f = llvm::Function::Create(ft,
				llvm::Function::InternalLinkage,
				function->name, compiler.module);
		function->function = f;

		/* Bind the argument symbols to their LLVM values. */

		{
			llvm::Function::arg_iterator vi = f->arg_begin();

			/* First, normal parameters. */

			typename vector<VariableSymbol*>::const_iterator ai = arguments.begin();
			while (ai != arguments.end())
			{
				VariableSymbol* symbol = *ai;
				assert(vi != f->arg_end());
				vi->setName(symbol->name + "." + symbol->hash);
				symbol->value = vi;

				ai++;
				vi++;
			}

			/* Now import any upvalues. */

			typename FunctionSymbol::LocalsMap::const_iterator li = function->locals.begin();

			while (li != function->locals.end())
			{
				if (li->first != li->second)
				{
					assert(vi != f->arg_end());
					VariableSymbol* symbol = li->second;
					vi->setName(symbol->name + "." + symbol->hash);
					symbol->value = vi;

					vi++;
				}

				li++;
			}

			assert(vi == f->arg_end());
		}

		/* Generate the code. */

		llvm::BasicBlock* toplevel = llvm::BasicBlock::Create(
				compiler.context, "", f);

		llvm::BasicBlock* bb = compiler.builder.GetInsertBlock();
		llvm::BasicBlock::iterator bi = compiler.builder.GetInsertPoint();
		compiler.builder.SetInsertPoint(toplevel);

		llvm::Value* v = body->codegen(compiler);
		compiler.builder.CreateRet(v);
		if (v->getType() != returntype)
		{
			std::stringstream s;
			s << "function is declared to return a "
			  << compiler.types->find(returntype)->name
			  << " but actually returns a "
			  << compiler.types->find(v->getType())->name;

			throw TypeException(s.str(), this);
		}

		compiler.builder.SetInsertPoint(bb, bi);

		return f;
	}
};

struct ASTToplevel : public ASTFunctionBody
{
	ToplevelSymbol* toplevel;

	using ASTNode::position;
	using ASTFrame::symbolTable;
	using ASTFunctionBody::function;
	using ASTFunctionBody::body;

	ASTToplevel(const Position& position, ToplevelSymbol* toplevel,
			ASTNode* body, SymbolTable* st):
		ASTFunctionBody(position, toplevel, body),
		toplevel(toplevel)
	{
		symbolTable = st;
	}

	void resolveVariables(Compiler& compiler)
	{
		body->resolveVariables(compiler);
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		llvm::Value* v = body->codegen(compiler);
		if (v)
		{
			std::stringstream s;
			s << "toplevel code must end in a 'return' statement";
			throw CompilationException(position.formatError(s.str()));
		}
		compiler.builder.CreateRetVoid();

		return NULL;
	}
};

struct ASTReturn : public ASTNode
{
	using ASTNode::position;
	using ASTNode::getFunction;
	using ASTNode::getFrame;

	ASTReturn(const Position& position):
		ASTNode(position)
	{
	}

	void resolveVariables(Compiler& compiler)
	{
		ToplevelSymbol* toplevel = getFunction()->isToplevel();
		if (!toplevel)
		{
			std::stringstream s;
			s << "'return' can only be used in top level code";
			throw CompilationException(position.formatError(s.str()));
		}
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		ToplevelSymbol* toplevel = getFunction()->isToplevel();
		assert(toplevel); /* checked in resolveVariables */

		/* Find the pointer to the output block. */

		llvm::Function* f = toplevel->function;
		
		/* Copy out output values. */

		SymbolTable* symbolTable = getFrame()->symbolTable;
		for (unsigned i=0; i<toplevel->returns.size(); i++)
		{
			VariableSymbol* outsym = toplevel->returns[i]->isVariable();
			assert(outsym);
			llvm::Value* ptr = outsym->value;

			Symbol* insym = symbolTable->resolve(outsym->name);
			if (!insym)
			{
				std::stringstream s;
				s << "output value '" << outsym->name << "' was not set";
				throw CompilationException(position.formatError(s.str()));
			}

			llvm::Value* value = insym->isValued()->emitValue(compiler);
			if (outsym->type->asVector())
				outsym->type->asVector()->storeToArray(value, ptr);
			else
				compiler.builder.CreateStore(value, ptr);
		}

		return NULL;
	}
};

struct ASTDefineFunction : public ASTFrame
{
	FunctionSymbol* function;
	ASTNode* definition;
	ASTNode* body;

	using ASTNode::parent;
	using ASTNode::getFunction;
	using ASTFrame::symbolTable;

	ASTDefineFunction(const Position& position, FunctionSymbol* function,
			ASTNode* definition, ASTNode* body):
		ASTFrame(position),
		function(function), definition(definition), body(body)
	{
		definition->parent = body->parent = this;
	}

	void resolveVariables(Compiler& compiler)
	{
		symbolTable = compiler.retain(new SingletonSymbolTable(
				parent->getFrame()->symbolTable));
		symbolTable->add(function);
		function->parent = getFunction();

		definition->resolveVariables(compiler);
		body->resolveVariables(compiler);
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		definition->codegen(compiler);
		return body->codegen(compiler);
	}
};

struct ASTFunctionCall : public ASTNode
{
	string id;
	vector<ASTNode*> arguments;
	CallableSymbol* function;

	using ASTNode::position;
	using ASTNode::getFrame;
	using ASTNode::getFunction;

	ASTFunctionCall(const Position& position, const string& id,
			const vector<ASTNode*>& arguments):
		ASTNode(position),
		id(id), arguments(arguments),
		function(NULL)
	{
		for (typename vector<ASTNode*>::const_iterator i = arguments.begin(),
				e = arguments.end(); i != e; i++)
		{
			(*i)->parent = this;
		}
	}

	void resolveVariables(Compiler& compiler)
	{
		Symbol* symbol = getFrame()->symbolTable->resolve(id);
		if (!symbol)
			throw SymbolException(id, this);
		function = symbol->isCallable();
		if (!function)
		{
			std::stringstream s;
			s << "attempt to call '" << id << "', which is not a function";
			throw CompilationException(position.formatError(s.str()));
		}

		for (typename vector<ASTNode*>::const_iterator i = arguments.begin(),
				e = arguments.end(); i != e; i++)
		{
			(*i)->resolveVariables(compiler);
		}

		/* Ensure that any upvalues that the function wants exist in this
		 * function.
		 */

		FunctionSymbol* callee = function->isFunction();
		if (callee)
		{
			FunctionSymbol* caller = getFunction();
			for (typename FunctionSymbol::LocalsMap::const_iterator i = callee->locals.begin(),
					e = callee->locals.end(); i != e; i++)
			{
				if (i->first != i->second)
					caller->importUpvalue(compiler, i->first);
			}
		}
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		function->checkParameterCount(compiler, arguments.size());

		/* Formal parameters. */

		vector<llvm::Value*> parameters;
		for (typename vector<ASTNode*>::const_iterator i = arguments.begin(),
				e = arguments.end(); i != e; i++)
		{
			llvm::Value* v = (*i)->codegen(compiler);
			parameters.push_back(v);
		}

		/* ...followed by imported upvalues. */

		FunctionSymbol* callee = function->isFunction();
		if (callee)
		{
			FunctionSymbol* caller = getFunction();
			for (typename FunctionSymbol::LocalsMap::const_iterator i = callee->locals.begin(),
					e = callee->locals.end(); i != e; i++)
			{
				if (i->first != i->second)
				{
					VariableSymbol* s = caller->locals[i->first];
					assert(s);
					parameters.push_back(s->value);
				}
			}
		}

		compiler.position = position;
		return function->emitCall(compiler, parameters);
	}
};

struct ASTCondition : public ASTNode
{
	ASTNode* condition;
	ASTNode* trueval;
	ASTNode* falseval;

	using ASTNode::position;

	ASTCondition(const Position& position, ASTNode* condition,
			ASTNode* trueval, ASTNode* falseval):
		ASTNode(position),
		condition(condition), trueval(trueval), falseval(falseval)
	{
		condition->parent = trueval->parent = falseval->parent = this;
	}

	void resolveVariables(Compiler& compiler)
	{
		condition->resolveVariables(compiler);
		trueval->resolveVariables(compiler);
		falseval->resolveVariables(compiler);
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		llvm::Value* cv = condition->codegen_to_boolean(compiler);

		llvm::BasicBlock* bb = compiler.builder.GetInsertBlock();

		llvm::BasicBlock* trueblock = llvm::BasicBlock::Create(
				compiler.context, "", bb->getParent());
		llvm::BasicBlock* falseblock = llvm::BasicBlock::Create(
				compiler.context, "", bb->getParent());
		llvm::BasicBlock* mergeblock = llvm::BasicBlock::Create(
				compiler.context, "", bb->getParent());

		compiler.builder.CreateCondBr(cv, trueblock, falseblock);

		compiler.builder.SetInsertPoint(trueblock);
		llvm::Value* trueresult = trueval->codegen(compiler);
		trueblock = compiler.builder.GetInsertBlock();
		compiler.builder.CreateBr(mergeblock);

		compiler.builder.SetInsertPoint(falseblock);
		llvm::Value* falseresult = falseval->codegen(compiler);
		falseblock = compiler.builder.GetInsertBlock();
		compiler.builder.CreateBr(mergeblock);

		if (!trueresult || !falseresult)
		{
			std::stringstream s;
			s << "you can't use 'return' inside conditionals";
			throw CompilationException(position.formatError(s.str()));
		}

		if (trueresult->getType() != falseresult->getType())
		{
			std::stringstream s;
			s << "the true and false value of a conditional must be the same type";
			throw CompilationException(position.formatError(s.str()));
		}

		compiler.builder.SetInsertPoint(mergeblock);
		llvm::PHINode* phi = compiler.builder.CreatePHI(trueresult->getType(), 2);
		phi->addIncoming(trueresult, trueblock);
		phi->addIncoming(falseresult, falseblock);
		return phi;
	}
};


#endif
