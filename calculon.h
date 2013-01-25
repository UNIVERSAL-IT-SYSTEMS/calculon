#ifndef CALCULON_H
#define CALCULON_H

#include <stdexcept>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <sstream>
#include <cassert>
#include <cctype>
#include <memory>
#include <boost/thread/tss.hpp>
#include "llvm/DerivedTypes.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JIT.h"
#include "llvm/IRBuilder.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/PassManager.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/DataLayout.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

namespace Calculon
{
	using std::string;
	using std::vector;
	using std::map;
	using std::pair;
	using std::set;
	using std::auto_ptr;

	class SettingsBase
	{
	public:
		enum
		{
			VECTOR = 'V',
			BOOLEAN = 'B'
		};
	};

	class RealIsDouble : public SettingsBase
	{
	public:
		enum
		{
			REAL = 'D'
		};

		typedef double Real;

	public:
		static llvm::Type* createRealType(llvm::LLVMContext& context)
		{
			return llvm::Type::getDoubleTy(context);
		}

		template <typename T>
		static T chooseDoubleOrFloat(T d, T f)
		{
			return d;
		}
	};

	class RealIsFloat : public SettingsBase
	{
	public:
		enum
		{
			REAL = 'F'
		};

		typedef float Real;

	public:
		static llvm::Type* createRealType(llvm::LLVMContext& context)
		{
			return llvm::Type::getFloatTy(context);
		}

		template <typename T>
		static T chooseDoubleOrFloat(T d, T f)
		{
			return f;
		}
	};

	#include "calculon_allocator.h"

	template <class S>
	class Instance
	{
	public:
		typedef typename S::Real Real;
		typedef struct
		{
			Real x, y, z;
		}
		Vector;

	private:
		enum
		{
			REAL = S::REAL,
			VECTOR = S::VECTOR
		};

	private:
		class CompilationException : public std::invalid_argument
		{
		public:
			CompilationException(const string& what):
				std::invalid_argument(what)
			{
			}
		};

		struct Position
		{
			int line;
			int column;

			string formatError(const string& what)
			{
				std::stringstream s;
				s << what
					<< " at " << line << ":" << column;
				return s.str();
			}
		};

		class CompilerState : public Allocator
		{
		public:
			llvm::LLVMContext& context;
			llvm::Module* module;
			llvm::IRBuilder<> builder;
			Position position;
			llvm::Type* intType;
			llvm::Value* xindex;
			llvm::Value* yindex;
			llvm::Value* zindex;
			llvm::Type* realType;
			llvm::Type* vectorType;
			llvm::Type* pointerType;
			llvm::Type* booleanType;

			CompilerState(llvm::LLVMContext& context, llvm::Module* module):
				context(context),
				module(module),
				builder(context),
				intType(NULL), xindex(NULL), yindex(NULL), zindex(NULL),
				realType(NULL), vectorType(NULL), pointerType(NULL)
			{
			}

			llvm::Type* getInternalType(char c)
			{
				switch (c)
				{
					case S::REAL:
						return realType;

					case S::VECTOR:
						return vectorType;

					case S::BOOLEAN:
						return booleanType;
				}
				assert(false);
			}

			llvm::Type* getExternalType(char c)
			{
				switch (c)
				{
					case S::VECTOR:
						return pointerType;

					default:
						return getInternalType(c);
				}
				assert(false);
			}
		};

		#include "calculon_symbol.h"
	public:
		#include "calculon_intrinsics.h"
	private:
		#include "calculon_lexer.h"
		#include "calculon_compiler.h"

	public:
		template <typename FuncType>
		class Program
		{
		private:
			llvm::LLVMContext _context;
			SymbolTable& _symbols;
			llvm::Module* _module;
			llvm::ExecutionEngine* _engine;
			llvm::Function* _function;
			FuncType* _funcptr;

		public:
			typedef typename S::Real Real;

		public:
			Program(SymbolTable& symbols, const string& code, const string& signature):
					_symbols(symbols),
					_funcptr(NULL)
			{
				std::istringstream stream(code);
				init(stream, signature);
			}

			Program(SymbolTable& symbols, std::istream& code, const string& signature):
					_symbols(symbols),
					_funcptr(NULL)
			{
				init(code, signature);
			}

			~Program()
			{
			}

			operator FuncType* () const
			{
				return _funcptr;
			}

			void dump()
			{
				_module->dump();
			}

		private:
			void init(std::istream& codestream, const string& signature)
			{
				_module = new llvm::Module("Calculon Function", _context);

				llvm::InitializeNativeTarget();

				llvm::TargetOptions options;
				options.PrintMachineCode = true;
				options.UnsafeFPMath = true;
				options.RealignStack = true;
				options.LessPreciseFPMADOption = true;
				options.GuaranteedTailCallOpt = true;
				options.AllowFPOpFusion = llvm::FPOpFusion::Fast;

				string s;
				_engine = llvm::EngineBuilder(_module)
					.setErrorStr(&s)
					.setOptLevel(llvm::CodeGenOpt::Aggressive)
					.setTargetOptions(options)
					.create();
				if (!_engine)
					throw CompilationException(s);
				_engine->DisableLazyCompilation();
	//			_engine->DisableSymbolSearching();

				Compiler compiler(_context, _module);

				/* Compile the program. */

				std::istringstream signaturestream(signature);
				FunctionSymbol* f = compiler.compile(signaturestream, codestream,
						&_symbols);

				/* Create the interface function from this signature. */

				const vector<VariableSymbol*>& arguments = f->arguments;
				vector<llvm::Type*> externaltypes;

				llvm::Type* returntype = compiler.getExternalType(f->returntype);
				bool inputoffset = false;
				if (f->returntype == S::VECTOR)
				{
					/* Insert an argument at the front which is the return-by-
					 * reference vector return value.
					 */

					externaltypes.push_back(compiler.getExternalType(S::VECTOR));
					returntype = llvm::Type::getVoidTy(_context);
					inputoffset = true;
				}

				for (int i=0; i<arguments.size(); i++)
				{
					VariableSymbol* symbol = arguments[i];
					externaltypes.push_back(compiler.getExternalType(symbol->type));
				}

				llvm::FunctionType* ft = llvm::FunctionType::get(
						returntype, externaltypes, false);

				_function = llvm::Function::Create(ft,
						llvm::Function::ExternalLinkage,
						"Entrypoint", _module);

				llvm::BasicBlock* bb = llvm::BasicBlock::Create(_context, "entry", _function);
				compiler.builder.SetInsertPoint(bb);

				/* Marshal the external types to the internal types. */

				vector<llvm::Value*> params;

				{
					int i = 0;
					llvm::Function::arg_iterator ii = _function->arg_begin();
					if (inputoffset)
						ii++;
					while (ii != _function->arg_end())
					{
						llvm::Value* v = ii;
						VariableSymbol* symbol = arguments[i];

						v->setName(symbol->name);
						if (symbol->type == S::VECTOR)
							v = compiler.loadVector(v);

						params.push_back(v);
						i++;
						ii++;
					}
				}

				/* Call the internal function. */

				llvm::Value* retval = f->emitCall(compiler, params);

				if (f->returntype == S::VECTOR)
				{
					compiler.storeVector(retval, _function->arg_begin());
					retval = NULL;
				}

				compiler.builder.CreateRet(retval);

				generate_machine_code();
			}

		private:

			void generate_machine_code()
			{
//				_module->dump();

				llvm::FunctionPassManager fpm(_module);
				llvm::PassManager mpm;
				llvm::PassManagerBuilder pmb;
				pmb.OptLevel = 3;
				pmb.populateFunctionPassManager(fpm);

				pmb.Inliner = llvm::createFunctionInliningPass(275);
				pmb.populateModulePassManager(mpm);

				fpm.doInitialization();
				llvm::verifyFunction(*_function);
				fpm.run(*_function);
				mpm.run(*_module);

				_funcptr = (FuncType*) _engine->getPointerToFunction(_function);
				assert(_funcptr);
			}
		};
	};
}

#endif
