/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <string>
#include <map>
#include <vector>
#include <list>
#include <memory>
#include <set>
#include <ctime>

#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/framework/op_gen_lib.h"
#include "tensorflow/java/src/gen/cc/java_defs.h"
#include "tensorflow/java/src/gen/cc/source_writer.h"
#include "tensorflow/java/src/gen/cc/op_generator.h"
#include "tensorflow/java/src/gen/cc/op_specs.h"

namespace tensorflow {
namespace java {
namespace {

const char* kLicenseSnippet =
    "tensorflow/java/src/gen/resources/license.java.snippet";

enum RenderMode {
  DEFAULT,
  SINGLE_OUTPUT,
  SINGLE_LIST_OUTPUT
};

inline void AddArgument(const Variable& var, const string& description,
    Method* method_out, Javadoc* javadoc_out) {
  method_out->add_argument(var);
  javadoc_out->add_param_tag(var.name(), description);
}

void CollectOpDependencies(const OpSpec& op, RenderMode mode,
    std::list<Type>* out) {
  out->push_back(Type::Class("Operation", "org.tensorflow"));
  out->push_back(Type::Class("OperationBuilder", "org.tensorflow"));
  out->push_back(Type::Class("Scope", "org.tensorflow.op"));
  if (mode == SINGLE_OUTPUT) {
    out->push_back(Type::Class("Output", "org.tensorflow"));
  } else if (mode == SINGLE_LIST_OUTPUT) {
    out->push_back(Type::Interface("Iterator", "java.util"));
  }
  // Don't pay attention to duplicate types in the dependency list, they will
  // be filtered out by the SourceWriter.
  for (const ArgumentSpec& input : op.inputs()) {
    out->push_back(input.var().type());
    if (input.iterable()) {
      out->push_back(Type::Class("Operands", "org.tensorflow.op"));
    }
  }
  for (const ArgumentSpec& output : op.outputs()) {
    out->push_back(output.var().type());
    if (output.iterable()) {
      out->push_back(Type::Class("Arrays", "java.util"));
    }
  }
  for (const AttributeSpec& attribute : op.attributes()) {
    out->push_back(attribute.var().type());
    out->push_back(attribute.jni_type());
  }
  for (const AttributeSpec& optional_attribute : op.optional_attributes()) {
    out->push_back(optional_attribute.var().type());
  }
}

void WriteSetAttrDirective(const AttributeSpec& attr, bool optional,
    SourceWriter* writer) {
  string var_name = optional ? "opts." + attr.var().name() : attr.var().name();
  if (attr.iterable()) {
    string array_name = attr.var().name() + "Array";
    writer->AppendType(attr.jni_type())
        .Append("[] " + array_name + " = new ")
        .AppendType(attr.jni_type())
        .Append("[" + var_name + ".size()];")
        .EndLine()
        .BeginBlock("for (int i = 0; i < " + array_name + ".length; ++i)")
        .Append(array_name + "[i] = ");
    if (attr.type().kind() == Type::GENERIC) {
      writer->Append("DataType.fromClass(" + var_name + ".get(i));");
    } else {
      writer->Append(var_name + ".get(i);");
    }
    writer->EndLine()
        .EndBlock()
        .Append("opBuilder.setAttr(\"" + attr.op_def_name() + "\", ")
        .Append(array_name + ");")
        .EndLine();
  } else {
    writer->Append("opBuilder.setAttr(\"" + attr.op_def_name() + "\", ");
    if (attr.var().type().name() == "Class") {
      writer->Append("DataType.fromClass(" + var_name + "));");
    } else {
      writer->Append(var_name + ");");
    }
    writer->EndLine();
  }
}

void RenderFactoryMethods(const OpSpec& op, const Type& op_class,
    SourceWriter* writer) {
  Method factory = Method::Create("create", op_class);
  Javadoc factory_doc = Javadoc::Create(
      "Factory method to create a class to wrap a new " + op_class.name()
      + " operation to the graph.");
  Variable scope =
      Variable::Create("scope", Type::Class("Scope", "org.tensorflow.op"));
  AddArgument(scope, "current graph scope", &factory, &factory_doc);
  for (const ArgumentSpec& input : op.inputs()) {
    AddArgument(input.var(), input.description(), &factory, &factory_doc);
  }
  for (const AttributeSpec& attr : op.attributes()) {
    AddArgument(attr.var(), attr.description(), &factory, &factory_doc);
  }
  if (!op.optional_attributes().empty()) {
    AddArgument(Variable::Varargs("options", Type::Class("Options")),
        "carries optional attributes values", &factory, &factory_doc);
  }
  factory_doc.add_tag("return", "a new instance of " + op_class.name());

  writer->BeginMethod(factory, PUBLIC|STATIC, &factory_doc);
  writer->Append("OperationBuilder opBuilder = scope.graph().opBuilder(\""
      + op.graph_op_name() + "\", scope.makeOpName(\""
      + op_class.name() + "\"));");
  writer->EndLine();
  for (const ArgumentSpec& input : op.inputs()) {
    if (input.iterable()) {
      writer->Append("opBuilder.addInputList(Operands.asOutputs("
          + input.var().name() + "));");
      writer->EndLine();
    } else {
      writer->Append("opBuilder.addInput(" + input.var().name()
          + ".asOutput());");
      writer->EndLine();
    }
  }
  for (const AttributeSpec& attribute : op.attributes()) {
    WriteSetAttrDirective(attribute, false, writer);
  }
  if (!op.optional_attributes().empty()) {
    writer->BeginBlock("if (options != null)")
        .BeginBlock("for (Options opts : options)");
    for (const AttributeSpec& attribute : op.optional_attributes()) {
      writer->BeginBlock("if (opts." + attribute.var().name() + " != null)");
      WriteSetAttrDirective(attribute, true, writer);
      writer->EndBlock();
    }
    writer->EndBlock().EndBlock();
  }
  writer->Append("return new ")
      .AppendType(op_class)
      .Append("(opBuilder.build());")
      .EndLine();
  writer->EndMethod();
}

void RenderConstructor(const OpSpec& op, const Type& op_class,
    SourceWriter* writer) {
  Variable operation =
      Variable::Create("operation", Type::Class("Operation", "org.tensorflow"));
  Method constructor = Method::ConstructorFor(op_class).add_argument(operation);
  for (const ArgumentSpec& output : op.outputs()) {
    if (output.iterable() && !output.type().unknown()) {
      constructor.add_annotation(
          Annotation::Create("SuppressWarnings").attributes("\"unchecked\""));
      break;
    }
  }
  writer->BeginMethod(constructor, PRIVATE)
      .Append("super(operation);")
      .EndLine();
  if (op.outputs().size() > 0) {
    writer->Append("int outputIdx = 0;")
        .EndLine();
    for (const ArgumentSpec& output : op.outputs()) {
      if (output.iterable()) {
        string var_length = output.var().name() + "Length";
        writer->Append("int " + var_length)
            .Append(" = operation.outputListLength(\"" + output.op_def_name()
                + "\");")
            .EndLine()
            .Append(output.var().name() + " = Arrays.asList(");
        if (!output.type().unknown()) {
          writer->Append("(")
              .AppendType(output.var().type().parameters().front())
              .Append("[])");
        }
        writer->Append("operation.outputList(outputIdx, " + var_length + "));")
            .EndLine()
            .Append("outputIdx += " + var_length + ";")
            .EndLine();
      } else {
        writer->Append(output.var().name()
                + " = operation.output(outputIdx++);")
            .EndLine();
      }
    }
  }
  writer->EndMethod();
}

void RenderGettersAndSetters(const OpSpec& op, SourceWriter* writer) {
  for (const AttributeSpec& attr : op.optional_attributes()) {
    Method setter =
        Method::Create(attr.var().name(), Type::Class("Options"));
    Javadoc setter_doc = Javadoc::Create();
    AddArgument(attr.var(), attr.description(), &setter, &setter_doc);
    writer->BeginMethod(setter, PUBLIC|STATIC, &setter_doc)
        .Append("return new Options()." + attr.var().name() + "("
            + attr.var().name() + ");")
        .EndLine()
        .EndMethod();
  }
  for (const ArgumentSpec& output : op.outputs()) {
    Method getter = Method::Create(output.var().name(), output.var().type());
    Javadoc getter_doc = Javadoc::Create(output.description());
    writer->BeginMethod(getter, PUBLIC, &getter_doc)
        .Append("return " + output.var().name() + ";")
        .EndLine()
        .EndMethod();
  }
}

void RenderInterfaceImpl(const OpSpec& op, RenderMode mode,
    SourceWriter* writer) {
  ArgumentSpec output = op.outputs().front();

  if (mode == SINGLE_OUTPUT) {
    bool cast2obj = output.type().unknown();
    Type return_type = Type::Class("Output", "org.tensorflow")
        .add_parameter(cast2obj ? Type::Class("Object") : output.type());
    Method as_output = Method::Create("asOutput", return_type)
        .add_annotation(Annotation::Create("Override"));
    if (cast2obj) {
      as_output.add_annotation(
          Annotation::Create("SuppressWarnings").attributes("\"unchecked\""));
    }
    writer->BeginMethod(as_output, PUBLIC);
    if (cast2obj) {
      writer->Append("return (").AppendType(return_type).Append(") ");
    } else {
      writer->Append("return ");
    }
    writer->Append(output.var().name() + ";")
        .EndLine()
        .EndMethod();

  } else if (mode == SINGLE_LIST_OUTPUT) {
    Type operand = Type::Interface("Operand", "org.tensorflow");
    if (output.type().unknown()) {
      operand.add_parameter(Type::Class("Object"));
    } else {
      operand.add_parameter(output.type());
    }
    Type return_type = Type::Interface("Iterator", "java.util")
        .add_parameter(operand);
    Method iterator = Method::Create("iterator", return_type)
        .add_annotation(Annotation::Create("Override"))
        .add_annotation(Annotation::Create("SuppressWarnings")
            .attributes("{\"rawtypes\", \"unchecked\"}"));
    // cast the output list using a raw List
    writer->BeginMethod(iterator, PUBLIC)
        .Append("return (" + return_type.name() + ") ")
        .Append(output.var().name() + ".iterator();")
        .EndLine()
        .EndMethod();
  }
}

void RenderOptionsClass(const OpSpec& op, const Type& op_class,
    SourceWriter* writer) {
  Type options_class = Type::Class("Options");
  Javadoc options_doc = Javadoc::Create(
      "Optional attributes for {@link " + op_class.full_name() + "}");
  writer->BeginInnerType(options_class, PUBLIC | STATIC, &options_doc);
  for (const AttributeSpec& attr : op.optional_attributes()) {
    Method setter = Method::Create(attr.var().name(), options_class);
    Javadoc setter_doc = Javadoc::Create();
    AddArgument(attr.var(), attr.description(), &setter, &setter_doc);
    writer->BeginMethod(setter, PUBLIC, &setter_doc)
        .Append("this." + attr.var().name() + " = " + attr.var().name() + ";")
        .EndLine()
        .Append("return this;")
        .EndLine()
        .EndMethod();
  }
  writer->EndLine();
  for (const AttributeSpec& optional_attribute : op.optional_attributes()) {
    writer->WriteField(optional_attribute.var(), PRIVATE);
  }
  Method constructor = Method::ConstructorFor(options_class);
  writer->BeginMethod(constructor, PRIVATE).EndMethod();
  writer->EndType();
}

inline Type ClassOf(const EndpointSpec& endpoint, const string& base_package) {
  return Type::Class(endpoint.name(),
      base_package + "." + str_util::Lowercase(endpoint.package()));
}

void GenerateOp(const OpSpec& op, const EndpointSpec& endpoint,
    const string& base_package, const string& output_dir, Env* env,
    const std::tm* timestamp) {
  Type op_class(ClassOf(endpoint, base_package)
      .add_supertype(Type::Class("PrimitiveOp", "org.tensorflow.op")));
  Javadoc op_javadoc(endpoint.javadoc());

  // op interfaces
  RenderMode mode = DEFAULT;
  if (op.outputs().size() == 1) {
    const ArgumentSpec& output = op.outputs().front();
    Type operand_type(output.type().unknown() ?
        Type::Class("Object") : output.type());
    Type operand_inf(Type::Interface("Operand", "org.tensorflow")
        .add_parameter(operand_type));
    if (output.iterable()) {
      mode = SINGLE_LIST_OUTPUT;
      op_class.add_supertype(Type::IterableOf(operand_inf));
    } else {
      mode = SINGLE_OUTPUT;
      op_class.add_supertype(operand_inf);
    }
  }
  // op generic parameters
  std::set<string> generics;
  for (const ArgumentSpec& output : op.outputs()) {
    if (output.type().kind() == Type::GENERIC && !output.type().unknown()
        && generics.find(output.type().name()) == generics.end()) {
      op_class.add_parameter(output.type());
      op_javadoc.add_param_tag("<" + output.type().name() + ">",
          "data type for {@code " + output.var().name() + "()} output");
      generics.insert(output.type().name());
    }
  }
  // op annotations
  char date[20];
  strftime(date, sizeof date, "%FT%TZ", timestamp);
  op_class.add_annotation(Annotation::Create("Generated", "javax.annotation")
      .attributes(string("value = \"op_generator\", date = \"") + date + "\""));
  if (endpoint.deprecated()) {
    op_class.add_annotation(Annotation::Create("Deprecated"));
    string explanation;
    if (!op.endpoints().front().deprecated()) {
      explanation = "use {@link " +
          ClassOf(op.endpoints().front(), base_package).full_name()
          + "} instead";
    } else {
      explanation = op.deprecation_explanation();
    }
    op_javadoc.add_tag("deprecated", explanation);
  }
  if (!op.hidden()) {
    // expose the op in the Ops Graph API only if it is visible
    op_class.add_annotation(
        Annotation::Create("Operator", "org.tensorflow.op.annotation")
          .attributes("group = \"" + endpoint.package() + "\""));
  }
  // create op class file
  string op_dir = io::JoinPath(output_dir,
      str_util::StringReplace(op_class.package(), ".", "/", true));
  if (!env->FileExists(op_dir).ok()) {
    TF_CHECK_OK(Env::Default()->RecursivelyCreateDir(op_dir));
  }
  std::unique_ptr<tensorflow::WritableFile> op_file;
  TF_CHECK_OK(env->NewWritableFile(
      io::JoinPath(op_dir, op_class.name() + ".java"), &op_file));

  // render endpoint source code
  SourceFileWriter writer(op_file.get());
  std::list<Type> dependencies;
  CollectOpDependencies(op, mode, &dependencies);
  writer.WriteFromFile(kLicenseSnippet)
      .EndLine()
      .BeginType(op_class, PUBLIC|FINAL, &dependencies, &op_javadoc);
  if (!op.optional_attributes().empty()) {
    RenderOptionsClass(op, op_class, &writer);
  }
  RenderFactoryMethods(op, op_class, &writer);
  RenderGettersAndSetters(op, &writer);
  if (mode != DEFAULT) {
    RenderInterfaceImpl(op, mode, &writer);
  }
  writer.EndLine();
  for (const ArgumentSpec& output : op.outputs()) {
    writer.WriteField(output.var(), PRIVATE);
  }
  RenderConstructor(op, op_class, &writer);
  writer.EndType();
}

}  // namespace

Status OpGenerator::Run(const OpList& op_list, const string& base_package,
    const string& output_dir) {
  ApiDefMap api_map(op_list);
  if (!api_dirs_.empty()) {
    // Only load api files that correspond to the requested "op_list"
    for (const auto& op : op_list.op()) {
      for (const auto& api_def_dir : api_dirs_) {
        const std::string api_def_file_pattern =
            io::JoinPath(api_def_dir, "api_def_" + op.name() + ".pbtxt");
        if (env_->FileExists(api_def_file_pattern).ok()) {
          TF_CHECK_OK(api_map.LoadFile(env_, api_def_file_pattern));
        }
      }
    }
  }
  api_map.UpdateDocs();
  time_t now;
  time(&now);
  for (const auto& op_def : op_list.op()) {
    const ApiDef* api_def = api_map.GetApiDef(op_def.name());
    if (api_def->visibility() != ApiDef::SKIP) {
      OpSpec op(OpSpec::Create(op_def, *api_def));
      for (const EndpointSpec& endpoint : op.endpoints()) {
        GenerateOp(op, endpoint, base_package, output_dir, env_, gmtime(&now));
      }
    }
  }
  return Status::OK();
}

}  // namespace java
}  // namespace tensorflow
