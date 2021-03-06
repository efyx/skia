/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrColorSpaceXform.h"
#include "SkColorSpace.h"
#include "SkColorSpacePriv.h"
#include "SkMatrix44.h"
#include "glsl/GrGLSLColorSpaceXformHelper.h"
#include "glsl/GrGLSLFragmentProcessor.h"
#include "glsl/GrGLSLFragmentShaderBuilder.h"

sk_sp<GrColorSpaceXform> GrColorSpaceXform::Make(SkColorSpace* src, SkColorSpace* dst) {
    // No transformation is performed in legacy mode, until SkColorSpaceXformCanvas is gone
    if (!dst) {
        return nullptr;
    }

    // Treat null sources as sRGB.
    if (!src) {
        src = sk_srgb_singleton();
    }

    // TODO: Plumb source alpha type
    SkColorSpaceXformSteps steps(src, kPremul_SkAlphaType, dst);

    return steps.flags.mask() == 0 ? nullptr  /* Noop transform */
                                   : sk_make_sp<GrColorSpaceXform>(steps);
}

sk_sp<GrColorSpaceXform> GrColorSpaceXform::MakeUnpremulToUnpremul(SkColorSpace* src,
                                                                   SkColorSpace* dst) {
    // No transformation is performed in legacy mode, until SkColorSpaceXformCanvas is gone
    if (!dst) {
        return nullptr;
    }

    // Treat null sources as sRGB.
    if (!src) {
        src = sk_srgb_singleton();
    }

    SkColorSpaceXformSteps steps = SkColorSpaceXformSteps::UnpremulToUnpremul(src, dst);

    return steps.flags.mask() == 0 ? nullptr  /* Noop transform */
                                   : sk_make_sp<GrColorSpaceXform>(steps);
}

bool GrColorSpaceXform::Equals(const GrColorSpaceXform* a, const GrColorSpaceXform* b) {
    if (a == b) {
        return true;
    }

    if (!a || !b || a->fSteps.flags.mask() != b->fSteps.flags.mask()) {
        return false;
    }

    if (a->fSteps.flags.linearize &&
        0 != memcmp(&a->fSteps.srcTF, &b->fSteps.srcTF, sizeof(a->fSteps.srcTF))) {
        return false;
    }

    if (a->fSteps.flags.gamut_transform &&
        0 != memcmp(&a->fSteps.src_to_dst_matrix, &b->fSteps.src_to_dst_matrix,
                    sizeof(a->fSteps.src_to_dst_matrix))) {
        return false;
    }

    if (a->fSteps.flags.encode &&
        0 != memcmp(&a->fSteps.dstTFInv, &b->fSteps.dstTFInv, sizeof(a->fSteps.dstTFInv))) {
        return false;
    }

    return true;
}

GrColor4f GrColorSpaceXform::apply(const GrColor4f& srcColor) {
    GrColor4f result = srcColor;
    fSteps.apply(result.fRGBA);
    return result;
}

//////////////////////////////////////////////////////////////////////////////

class GrGLColorSpaceXformEffect : public GrGLSLFragmentProcessor {
public:
    void emitCode(EmitArgs& args) override {
        const GrColorSpaceXformEffect& csxe = args.fFp.cast<GrColorSpaceXformEffect>();
        GrGLSLFPFragmentBuilder* fragBuilder = args.fFragBuilder;
        GrGLSLUniformHandler* uniformHandler = args.fUniformHandler;

        fColorSpaceHelper.emitCode(uniformHandler, csxe.colorXform());

        if (this->numChildProcessors()) {
            SkString childColor("src_color");
            this->emitChild(0, &childColor, args);

            SkString xformedColor;
            fragBuilder->appendColorGamutXform(&xformedColor, childColor.c_str(), &fColorSpaceHelper);
            fragBuilder->codeAppendf("%s = %s * %s;", args.fOutputColor, xformedColor.c_str(),
                                     args.fInputColor);
        } else {
            if (nullptr == args.fInputColor) {
                args.fInputColor = "half4(1)";
            }
            SkString xformedColor;
            fragBuilder->appendColorGamutXform(&xformedColor, args.fInputColor, &fColorSpaceHelper);
            fragBuilder->codeAppendf("%s = %s;", args.fOutputColor, xformedColor.c_str());
        }
    }

private:
    void onSetData(const GrGLSLProgramDataManager& pdman,
                   const GrFragmentProcessor& processor) override {
        const GrColorSpaceXformEffect& csxe = processor.cast<GrColorSpaceXformEffect>();
        fColorSpaceHelper.setData(pdman, csxe.colorXform());
    }

    GrGLSLColorSpaceXformHelper fColorSpaceHelper;

    typedef GrGLSLFragmentProcessor INHERITED;
};

//////////////////////////////////////////////////////////////////////////////

GrColorSpaceXformEffect::GrColorSpaceXformEffect(std::unique_ptr<GrFragmentProcessor> child,
                                                 sk_sp<GrColorSpaceXform> colorXform)
        : INHERITED(kGrColorSpaceXformEffect_ClassID, OptFlags(child.get()))
        , fColorXform(std::move(colorXform)) {
    if (child) {
        this->registerChildProcessor(std::move(child));
    }
}

std::unique_ptr<GrFragmentProcessor> GrColorSpaceXformEffect::clone() const {
    std::unique_ptr<GrFragmentProcessor> child =
            this->numChildProcessors() ? this->childProcessor(0).clone() : nullptr;
    return std::unique_ptr<GrFragmentProcessor>(
            new GrColorSpaceXformEffect(std::move(child), fColorXform));
}

bool GrColorSpaceXformEffect::onIsEqual(const GrFragmentProcessor& s) const {
    const GrColorSpaceXformEffect& other = s.cast<GrColorSpaceXformEffect>();
    return GrColorSpaceXform::Equals(fColorXform.get(), other.fColorXform.get());
}

void GrColorSpaceXformEffect::onGetGLSLProcessorKey(const GrShaderCaps& caps,
                                                    GrProcessorKeyBuilder* b) const {
    b->add32(GrColorSpaceXform::XformKey(fColorXform.get()));
}

GrGLSLFragmentProcessor* GrColorSpaceXformEffect::onCreateGLSLInstance() const {
    return new GrGLColorSpaceXformEffect();
}

GrFragmentProcessor::OptimizationFlags GrColorSpaceXformEffect::OptFlags(
        const GrFragmentProcessor* child) {
    // TODO: Implement constant output for constant input
    if (child) {
        OptimizationFlags flags = kNone_OptimizationFlags;
        if (child->compatibleWithCoverageAsAlpha()) {
            flags |= kCompatibleWithCoverageAsAlpha_OptimizationFlag;
        }
        if (child->preservesOpaqueInput()) {
            flags |= kPreservesOpaqueInput_OptimizationFlag;
        }
        return flags;
    } else {
        return kCompatibleWithCoverageAsAlpha_OptimizationFlag |
               kPreservesOpaqueInput_OptimizationFlag;
    }
}

std::unique_ptr<GrFragmentProcessor> GrColorSpaceXformEffect::Make(SkColorSpace* src,
                                                                   SkColorSpace* dst) {
    auto xform = GrColorSpaceXform::Make(src, dst);
    if (!xform) {
        return nullptr;
    }

    return std::unique_ptr<GrFragmentProcessor>(new GrColorSpaceXformEffect(nullptr,
                                                                            std::move(xform)));
}

std::unique_ptr<GrFragmentProcessor> GrColorSpaceXformEffect::Make(
        std::unique_ptr<GrFragmentProcessor> child,
        SkColorSpace* src, SkColorSpace* dst) {
    if (!child) {
        return nullptr;
    }

    auto xform = GrColorSpaceXform::Make(src, dst);
    if (!xform) {
        return child;
    }

    return std::unique_ptr<GrFragmentProcessor>(new GrColorSpaceXformEffect(std::move(child),
                                                                            std::move(xform)));
}
