// This header file provides the RPOCO macro and runtime system that creates 
// template specialized runtime type data usable for serialization and similar tasks.


#ifndef __INCLUDED_RPOCO_HPP__
#define __INCLUDED_RPOCO_HPP__

#pragma once

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <utility>
#include <atomic>
#include <mutex>
#include <cctype>
#include <stdint.h>
#include <type_traits>
#include <functional>
#include <memory>

#include <iostream>

// Use the RPOCO macro within a compound definition to create
// automatic serialization information upon the specified members.
// RPOCO has thread safe typeinfo init (double checked lock) so using
// functions dependant of the functionality from multiple threads should
// be safe.

// Note 1: The macro magic below is necessary to unpack the field data and provide a coherent interface
// Note 2: This lib uses ptrdiffed offsets to place fields at runtime

#define RPOCO(...) \
	void rpoco_type_info_expand(rpoco::type_info *ti,std::vector<std::string>& names,int idx) {} \
	template<typename H,typename... R> \
	void rpoco_type_info_expand(rpoco::type_info *ti,std::vector<std::string>& names,int idx,H& head,R&... rest) {\
		ptrdiff_t off=(ptrdiff_t) (  ((uintptr_t)&head)-((uintptr_t)this) ); \
		ti->add(new rpoco::field< std::remove_reference<H>::type >(names[idx],off) );\
		rpoco_type_info_expand(ti,names,idx+1,rest...); \
	} \
	rpoco::type_info* rpoco_type_info_get() { \
		static rpoco::type_info ti; \
		if(!ti.is_init()) { \
			ti.init([this](rpoco::type_info *ti) { \
				std::vector<std::string> names=rpoco::extract_macro_names(#__VA_ARGS__); \
				rpoco_type_info_expand(ti,names,0,__VA_ARGS__); \
			} ); \
		} \
		return &ti; \
	}

// Actual rpoco namespace containing member information and templates for iteration
namespace rpoco {
	class member;
	class member_provider;

	struct niltarget {};

	// visitation is done in a similar way both during creation (deserialization) and querying (serialization)
	// vt_none is the result any querying system should provide when calling peek on the visitor while
	// creation routines should provide the type of the next data item to be input/read/creation.
	enum visit_type {
		vt_none,
		vt_error,
		vt_object,
		vt_array,
		vt_null,
		vt_bool,
		vt_number,
		vt_string
	};

	// subclass this type to enumerate data structures.
	struct visitor {
		virtual visit_type peek()=0; // return vt_none if querying objects, otherwise return the next data type.
		virtual bool consume(visit_type vt,std::function<void(std::string&)> out)=0; // used by members to start consuming data from complex input objects during creation
		virtual void produce_start(visit_type vt)=0; // used to start producing complex objects
		virtual void produce_end(visit_type vt)=0; // used to stop a production
		// the primitive types below are just visited the same way during both reading and creation
		virtual void visit_null() = 0;
		virtual void visit(bool& b)=0;
		virtual void visit(int& x)=0;
		virtual void visit(double& x)=0;
		virtual void visit(std::string &k)=0; // 
		virtual void visit(char *,size_t sz)=0;
	};

	// generic class type visitation template functionality.
	// if an object wants to override to handle multiple types a specialization
	// of this template can be done, see rpoco::niltarget or rpocojson::json_value
	template<typename F>
	struct visit { visit(visitor &v,F &f) {
		// get member info of a rpoco object
		member_provider *fp=f.rpoco_type_info_get();
		// if reading then start consuming data
		if (v.consume(vt_object,[&v,fp,&f](std::string& n){
				// check if the member to consume exists
				if (! fp->has(n)) {
					// if not start the nil consumer
					niltarget nt;
					rpoco::visit<niltarget>(v,nt);
				} else {
					// visit member
					(*fp)[n]->visit(v,(void*)&f);
				}
			}))
		{
			// nothing more to do post consumption
			return;
		} else {
			// we're in production mode so produce
			// data from our members
			v.produce_start(vt_object);
			for (int i=0;i<fp->size();i++) {
				v.visit((*fp)[i]->name());
				(*fp)[i]->visit(v,(void*)&f);
			}
			v.produce_end(vt_object);
		}
	}};

	// map visitation
	template<typename F>
	struct visit<std::map<std::string,F>> { visit(visitor &v,std::map<std::string,F> &mp) {
		if (v.consume(vt_object,[&v,&mp](std::string& x) {
				// just produce new entries during consumption
				rpoco::visit<F>(v, mp[x] );
			}))
		{
			return;
		} else {
			// production wanted, so produce all
			// members to a target object.
			v.produce_start(vt_object);
			for (std::pair<std::string,F> p:mp) {
				rpoco::visit<std::string>(v,p.first);
				rpoco::visit<F>(v,p.second);
			}
			v.produce_end(vt_object);
		}
	}};

	// nil visitor, this visitor
	// can consume any type thrown at it and is used
	// to ignore unknown incomming data
	template<>
	struct visit<niltarget> { visit(visitor &v,niltarget &nt) {
		visit_type vtn;
		switch(vtn=v.peek()) {
		case vt_null :
			v.visit_null();
			break;
		case vt_number : {
				double d;
				v.visit(d);
			} break;
		case vt_bool : {
				bool b;
				v.visit(b);
			} break;
		case vt_string : {
				std::string str;
				v.visit(str);
			} break;
		case vt_array :
		case vt_object : {
				v.consume(vtn,[&v,&nt](std::string& propname) {
					niltarget ntn;
					//std::cout<<"Ignoring prop:"<<propname<<"\n";
					rpoco::visit<niltarget>(v,ntn);
				});
			} break;
		}
	}};

	// vector visitor, used for arrays
	template<typename F>
	struct visit<std::vector<F>> { visit(visitor &v,std::vector<F> &vp) {
		if (v.consume(vt_array,[&v,&vp](std::string& x) {
				// consumption of incoming data
				vp.emplace_back();
				rpoco::visit<F>(v,vp.back());
			}))
		{
			return ;
		} else {
			// production of outgoing data
			v.produce_start(vt_array);
			for (F &f:vp) {
				rpoco::visit<F>(v,f);
			}
			v.produce_end(vt_array);
		}
	}};


	// the pointer visitor creates a new object of the specified type
	// during consumption so destructors should
	// always check for the presence and destroy if needed.
	template<typename F>
	struct visit<F*> { visit(visitor &v,F *& fp) {
		if (v.peek()!=vt_null && v.peek()!=vt_none && !fp) {
			fp=new F();
		}
		if (fp)
			visit<F>(v,*fp);
		else
			v.visit_null();
	}};

	// like the pointer consumer above the shared_ptr
	// consumer will also create new objects to hold if needed.
	template<typename F>
	struct visit<std::shared_ptr<F>> { visit(visitor &v,std::shared_ptr<F> & fp) {
		if (v.peek()!=vt_null && v.peek()!=vt_none && !fp) {
			fp.reset(new F());
		}
		if (fp)
			visit<F>(v,*fp);
		else
			v.visit_null();
	}};

	// a unique_ptr version of the above shared_ptr template
	template<typename F>
	struct visit<std::unique_ptr<F>> { visit(visitor &v,std::unique_ptr<F> & fp) {
		if (v.peek()!=vt_null && v.peek()!=vt_none && !fp) {
			fp.reset(new F());
		}
		if (fp)
			visit<F>(v,*fp);
		else
			v.visit_null();
	}};

	// integer visitation
	template<> struct visit<int> {
		visit(visitor &v,int &ip) {
			v.visit(ip);
		}
	};

	// double visitation
	template<> struct visit<double> {
		visit(visitor &v,double &ip) {
			v.visit(ip);
		}
	};

	// string visitation
	template<> struct visit<std::string> { visit(visitor &v,std::string &str) {
		v.visit(str);
	}};

	// sized C-string visitation
	template<int SZ> struct visit<char[SZ]> { visit(visitor &v,char (&str)[SZ]) {
		v.visit(str,SZ);
	}};

	// base class for class members, gives a name and provides an abstract visitation function
	class member {
	public:
	protected:
		std::string m_name;
	public:
		member(std::string name) {
			this->m_name=name;
		}
		std::string& name() {
			return m_name;
		}
		virtual void visit(visitor &v,void *p)=0;
	};

	// field class template for the actual members (see the RPOCO macro for usage)
	template<typename F>
	class field : public member {
		ptrdiff_t m_offset;
	public:
		field(std::string name,ptrdiff_t off) : member(name) {
			this->m_offset=off;
		}
		ptrdiff_t offset() {
			return m_offset;
		}
		virtual void visit(visitor &v,void *p) {
			rpoco::visit<F>(v,*(F*)( (uintptr_t)p+(ptrdiff_t)m_offset ));
		}
	};

	// a generic member provider class
	class member_provider {
	public:
		virtual int size()=0; // number of members
		virtual bool has(std::string id)=0; // do we have the requested member?
		virtual member*& operator[](int idx)=0; // get an indexed member (0-size() are valid indexes)
		virtual member*& operator[](std::string id)=0; // get a named member
	};

	// type_info is a member_provider implementation for regular classes.
	class type_info : public member_provider {
		std::vector<member*> fields;
		std::unordered_map<std::string,member*> m_named_fields;
		std::atomic<int> m_is_init;
		std::mutex init_mutex;
	public:
		virtual int size() {
			return fields.size();
		}
		virtual bool has(std::string id) {
			return m_named_fields.end()!=m_named_fields.find(id);
		}
		virtual member*& operator[](int idx) {
			return fields[idx];
		}
		virtual member*& operator[](std::string id) {
			return m_named_fields[id];
		}
		int is_init() {
			return m_is_init.load();
		}
		void init(std::function<void (type_info *ti)> initfun) {
			std::lock_guard<std::mutex> lock(init_mutex);\
			if (!m_is_init.load()) {
				initfun(this);
				m_is_init.store(1);
			}
		}
		void add(member *fb) {
			fields.push_back(fb);
			m_named_fields[fb->name()]=fb;
		}
	};

	// helper function to the RPOCO macro to parse the data
	static std::vector<std::string> extract_macro_names(const char *t) {
		// skip spaces and commas
		while(*t&&(std::isspace(*t)||*t==',')) { t++; }
		// token start pos
		const char *s=t;
		// the vector of output names
		std::vector<std::string> out;
		// small loop to extract tokens from non-space/comma characters
		while(*t) {
			if (*t==','||std::isspace(*t)) {
				out.push_back(std::string(s,t-s));
				// skip spaces and commas
				while(*t&&(std::isspace(*t)||*t==',')) { t++; }
				s=t;
			} else {
				t++;
			}
		}
		// did we have an extra string? push it.
		if (s!=t)
			out.push_back(std::string(s,t-s));
		return out;
	}
};

#endif // __INCLUDED_RPOCO_HPP__
