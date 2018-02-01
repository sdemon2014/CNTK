//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
// TODO:
//  - remove empty-matrix checks: if an op is well-defined with empty matrices, then do it
//  - Resize() must be cheap if it does nothing  (I already did that for CPU; already done for GPU?)
//

#pragma once

#include "Basics.h"
#include "File.h"
#include "CommonMatrix.h"
#include "TensorShape.h" // only for SmallVector; I was hoping to keep this out
#include "RNGHandle.h"
#include "DataTransferer.h"
#include <limits.h>
#include <memory> // for shared_ptr
#include <array>
#include <initializer_list>
#include "QuantizedOperations.h"

// Forward declarations
namespace CNTK
{
    class NDArrayView;
    class Value;
}

// This class is exported from the Math.dll
namespace Microsoft { namespace MSR { namespace CNTK {

enum CurrentDataLocation
{
    NONE,
    CPU,
    GPU,
    BOTH
};

enum MatrixType
{
    UNDETERMINED,
    DENSE,
    SPARSE
};

// avoid pulling in these header files for consumers of this class
template <class ElemType> class GPUMatrix;
template <class ElemType> class CPUMatrix;
template <class ElemType> class GPUSparseMatrix;
template <class ElemType> class CPUSparseMatrix;
template <class ElemType> class DeviceBoundNumber;

template <class ElemType>
class Matrix;

// <ElemType>-agnostic base class
struct /*interface*/ MATH_API MatrixBase   // : public ::CNTK::enable_strong_shared_ptr<MatrixBase>
{
    typedef std::shared_ptr<MatrixBase> MatrixBasePtr;
    //typedef ::CNTK::strong_shared_ptr<MatrixBase> MatrixBasePtr;
    virtual int GetDeviceId() const = 0;
    virtual MatrixType GetMatrixType() const = 0;
    virtual MatrixFormat GetFormat() const = 0;
    virtual size_t GetNumElements() const = 0;
    virtual size_t GetNumViews() const = 0;
    virtual void Reset() = 0; // reset for sparse matrix
    virtual void Resize1(const size_t numRows, const size_t numCols, const size_t numNZElemToReserve = 0, bool growOnly = true) = 0;
    // BUGBUG: ^^ This should just be Resize(), but that causes link errors for unknown reasons.
    virtual void CastAssignValuesOf(const MatrixBase& other) = 0; // allows for mixed assignment with conversion
    // TODO: Move more generic functions such as getting dims, resizing, and getting/setting as scalars in here.
    virtual ~MatrixBase();
    // helpers for casting
    template<class ElemType> const Matrix<ElemType>* AsPtr() const { return dynamic_cast<const Matrix<ElemType>*>(this); }
    template<class ElemType> const Matrix<ElemType>& AsRef() const { return dynamic_cast<const Matrix<ElemType>&>(*this); }
    template<class ElemType> bool ElemTypeIs() const { return dynamic_cast<const Matrix<ElemType>*>(this) != nullptr; }
};
typedef MatrixBase::MatrixBasePtr MatrixBasePtr;

// Note: To comply with BLAS libraries, matrices are stored in ColMajor. However, by default C/C++/C# use RowMajor convertion.
// !!!WARNING!!! This class is NOT THREAD SAFE. Test and add necessary modifications if using in multi-threaded environment
template <class ElemType>
class MATH_API Matrix : public MatrixBase
{
    friend class ::CNTK::NDArrayView;
    friend class ::CNTK::Value;

    typedef MatrixBase Base;
private:
    mutable BaseMatrix<ElemType>*                 m_baseMatrix;
#if 0 // this switches between shared_ptr and our own for the sub-objects
    mutable shared_ptr<GPUMatrix      <ElemType>> m_GPUMatrix;
    mutable shared_ptr<CPUMatrix      <ElemType>> m_CPUMatrix;
    mutable shared_ptr<GPUSparseMatrix<ElemType>> m_GPUSparseMatrix;
    mutable shared_ptr<CPUSparseMatrix<ElemType>> m_CPUSparseMatrix;
    template <typename T, typename ...CtorArgTypes>
    static inline std::shared_ptr<T> MakeSharedMatrixObject(CtorArgTypes&& ...ctorArgs)
    {
        return ::CNTK::MakeSharedObject<T>(std::forward<CtorArgTypes>(ctorArgs)...);
    }
#else
    mutable ::CNTK::strong_shared_ptr<GPUMatrix      <ElemType>> m_GPUMatrix;
    mutable ::CNTK::strong_shared_ptr<CPUMatrix      <ElemType>> m_CPUMatrix;
    mutable ::CNTK::strong_shared_ptr<GPUSparseMatrix<ElemType>> m_GPUSparseMatrix;
    mutable ::CNTK::strong_shared_ptr<CPUSparseMatrix<ElemType>> m_CPUSparseMatrix;
    template <typename T, typename ...CtorArgTypes>
    static inline ::CNTK::strong_shared_ptr<T> MakeSharedMatrixObject(CtorArgTypes&& ...ctorArgs)
    {
        return ::CNTK::MakeSharedObject1<T>(std::forward<CtorArgTypes>(ctorArgs)...);
    }
#endif

    mutable MatrixType m_matrixType;
    mutable CurrentDataLocation m_currentDataLocation; // Indicates which matrix is current

    mutable DEVICEID_TYPE m_preferredDeviceId;
    mutable size_t m_numTimesDeviceChanged;
    mutable size_t m_numTimesMatrixTypeChanged;
    mutable int m_devicesTransferedTo[2]; // TODO: what is this for? Seems only diagnostics
 
    // Moves matrix from device id_from to device with id_to. This method doesn't change preferred device Id
    void _transferFromDeviceToDevice(int id_from, int id_to, bool isBeingMoved = true, bool emptyTransfer = false) const;
    // Moves matrix from current device to device with id_to. This method doesn't change preferred device Id
    void _transferToDevice(int id_to, bool isBeingMoved = true, bool emptyTransfer = false) const;
    template <class ElemType2>
    static void DecideAndMoveToRightDevice(const Matrix<ElemType>& a, const Matrix<ElemType2>& b);
    static void DecideAndMoveToRightDevice(const Matrix<ElemType>& a, const Matrix<ElemType>& b, const Matrix<ElemType>& c);
    static void DecideAndMoveToRightDevice(const Matrix<ElemType>& a, const Matrix<ElemType>& b, const Matrix<ElemType>& c, const Matrix<ElemType>& d);
    static void DecideAndMoveToRightDevice(const Matrix<ElemType>& a, const Matrix<ElemType>& b, const Matrix<ElemType>& c, const Matrix<ElemType>& d, const Matrix<ElemType>& e);
    static void CopyElementsFromDenseToSparse(CPUMatrix<ElemType>& from, CPUSparseMatrix<ElemType>& dest);

public:
    typedef std::shared_ptr<Matrix<ElemType>> MatrixPtr;
    typedef std::shared_ptr<const Matrix<ElemType>> ConstMatrixPtr;
    //typedef ::CNTK::strong_shared_ptr<Matrix<ElemType>> MatrixPtr;
    //typedef ::CNTK::strong_shared_ptr<const Matrix<ElemType>> ConstMatrixPtr;
    // Constructors, destructors and other static matrix builders
    // Each constructor can take deviceId as parameter.
    // If deviceId<0 then the matrix will be based in RAM (CPUMatrix)
    // Elseif deviceId>=0 then the matrix will be based on GPU with specified deviceId
    explicit Matrix(DEVICEID_TYPE deviceId);
    // This constructor is not used, but it makes the ownership of baseMatrix ambiguous. If it's to be used, ensure that the semantics with external buffer are clear.
#if 0
    Matrix(shared_ptr<BaseMatrix<ElemType>> baseMatrix, ElemType* pArray, DEVICEID_TYPE deviceId);                                     // constructor for setting Matrix from a base matrix (externally managed butter pArray)
#endif
    // construct a new matrix
    Matrix(const size_t numRows, const size_t numCols, DEVICEID_TYPE deviceId, const MatrixType matrixType = DENSE, const MatrixFormat matrixFormat = matrixFormatDense, const size_t nnz = 0);

    // TODO: Rewrite this constructor to eliminate the external buffers flag. Make a separate construction mechanism for Matrix objects that don't own their storage.
    // construct a new matrix with external storage
    // If matrixFlags & matrixFlagDontOwnBuffer, then deleter can be specified.
    // TODO: This API does not actually work for sparse matrices, so the nnz parameter should be removed (not sure if this API is used incorrectly, relying on this).
    Matrix(const size_t numRows, const size_t numCols, ElemType* pArray, DEVICEID_TYPE deviceId, const size_t matrixFlags = matrixFlagNormal, const size_t nnz = 0, IBaseMatrixStorageExternalBufferDeleter* deleter = nullptr);

    Matrix(const Matrix<ElemType>& deepCopyFrom, DEVICEID_TYPE deviceId);
    Matrix(Matrix<ElemType>&& moveFrom);                                                    // move constructor, shallow copy
    Matrix<ElemType>& operator=(Matrix<ElemType>&& moveFrom);                               // move assignment operator, shallow copy

    Matrix<ElemType> DeepClone() const;

    // Disallow deep copy construction and assignment to avoid
    // inadvertent silent deep copying
    Matrix(const Matrix<ElemType>& deepCopyFrom) = delete;
    Matrix<ElemType>& operator=(const Matrix<ElemType>& deepCopyFrom) = delete;

    static Matrix<ElemType> Ones(const size_t rows, const size_t cols, DEVICEID_TYPE deviceId);
    static Matrix<ElemType> Zeros(const size_t rows, const size_t cols, DEVICEID_TYPE deviceId);
    static Matrix<ElemType> Eye(const size_t rows, DEVICEID_TYPE deviceId);

#define USE_TIME_BASED_SEED ULONG_MAX
    static Matrix<ElemType> RandomUniform(const size_t rows, const size_t cols, DEVICEID_TYPE deviceId, const ElemType low, const ElemType high, unsigned long seed = USE_TIME_BASED_SEED);
    static Matrix<ElemType> RandomGaussian(const size_t rows, const size_t cols, DEVICEID_TYPE deviceId, const ElemType mean, const ElemType sigma, unsigned long seed = USE_TIME_BASED_SEED);

    static void SetDevice(DEVICEID_TYPE deviceId); // TODO: unify with PrepareDevice()
    static double SyncDevice(DEVICEID_TYPE deviceId);

    void ReleaseMemory();
    ~Matrix();

    // workaround to bugs in BOTH implementation: force to collapse to home location
    void CollapseDataLocation() const
    {
        SetDataLocation(GetDeviceId() < 0 ? CurrentDataLocation::CPU : CurrentDataLocation::GPU, GetMatrixType());
    }

private:
    Matrix(const MatrixFlags matrixFlags, const MatrixType matrixType, const MatrixFormat matrixFormat, DEVICEID_TYPE deviceID); // only used internally to initialize a blank matrix
    Matrix(const MatrixFlags matrixFlags, const MatrixType matrixType, DEVICEID_TYPE deviceID);                                  // only used internally to initialize a blank matrix
    Matrix(const MatrixFlags matrixFlags, DEVICEID_TYPE deviceID);                                                               // only used internally to initialize a blank matrix
    void Init(DEVICEID_TYPE deviceID);
    void SetDataLocation(CurrentDataLocation location, MatrixType type = UNDETERMINED) const;
    void ShallowCopyFrom(const Matrix<ElemType>& other);
    void ShallowMoveFrom(Matrix<ElemType>&& other);

public:
#if 0
    // down-cast to make life easier
    template <class T>
    static shared_ptr<T> DownCast(shared_ptr<BaseMatrix<ElemType>> inode)
    {
        shared_ptr<T> node = dynamic_pointer_cast<T>(inode);
        if (!node)
            LogicError("A Matrix of mismatching type was passed.");
        return node;
    }
#endif

    MatrixType GetMatrixType() const final;
    MatrixFormat GetFormat() const final;
    bool OwnBuffer() const { return m_baseMatrix->OwnBuffer(); }
    size_t GetNumViews() const final; // (for debugging: how many Matrix objects share the same storage object)
    int GetDeviceId() const final; // -1 if CPU, otherwise GPU CUDA device id
    DEVICEID_TYPE GetPreferredDeviceId() const { return m_preferredDeviceId; }; // -1 if CPU, otherwise GPU CUDA device id
    void SetPreferredDeviceId(DEVICEID_TYPE preferredDeviceId) { m_preferredDeviceId = preferredDeviceId; }
    // Moves matrix from device id_from to device with id_to.
    // If emptyTransfer=true, then no data is ever moved, just corresponding GPU/CPU matrices are deleted and then created using empty constructor
    void TransferFromDeviceToDevice(int id_from, int id_to, bool isBeingMoved = false, /*if false then keep source and set location to BOTH*/ bool emptyTransfer = false, bool updatePreferredDevice = true) const;
    // Same as TransferFromDeviceToDevice() but moves only if it is currently not on the target device
    void TransferToDeviceIfNotThere(int id_to, bool isBeingMoved = false, bool emptyTransfer = false, bool updatePreferredDevice = true) const;
    CurrentDataLocation GetCurrentMatrixLocation() const { return m_currentDataLocation; };
    void SwitchToMatrixType(MatrixType newMatrixType, MatrixFormat newMatrixFormat, bool keepValues); // sets matrix type between dense and sparse
    size_t GetNumRows() const;
    size_t GetNumCols() const;
    size_t GetNumElements() const final;
    bool HasNoElements() const { return GetNumElements() == 0; }
    bool IsEmpty() const;
    size_t BufferSize() const;
    ElemType* Data() const;

    ElemType* CopyToArray() const;                                              // allocated by the callee but need to be deleted by the caller
    size_t CopyToArray(ElemType*& arrayCopyTo, size_t& currentArraySize) const; // allocated by the callee but need to be deleted by the caller
    size_t* TryCopyToArrayAsOneHot() const;                                     // allocated by the callee but need to be deleted by the caller
    // colStride specifies leading dimension of dst.
    // REVIEW alexeyk: GPU version copies from device to host only, implement all versions (device <-> host).
    void CopySection(size_t numRows, size_t numCols, ElemType* dst, size_t colStride) const;

    Matrix<ElemType> ColumnSlice(size_t startColumn, size_t numCols, size_t pretendSourceHasNumCols = 0) const; // note: 'const' is misleading here, as the returned matrix is a mutable reference

    // difference between AssignColumnSlice and SetColumnSlice
    // AssignColumnSlice :      this(:, startColumn:startColumn+numCols-1) = fromMatrix(:, startColumn: startColumn+numCols-1)
    // SetColumnSlice    :      this(:, startColumn:startColumn+numCols-1) = fromMatrix(:, 0: startColumn+numCols-1)
    // AssignColumnSlice does not transfer data, it uses external data
    // SetColumnSlice    copies data

    Matrix<ElemType>& AssignColumnSlice(const Matrix<ElemType>& fromMatrix, size_t startColumn, size_t numCols);
    Matrix<ElemType>& SetColumnSlice(const Matrix<ElemType>& fromMatrix, size_t startColumn, size_t numCols);

    void CopyColumnsStrided(const Matrix<ElemType>& fromMatrix, size_t numCols, size_t srcNumColsStride, size_t destNumColsStride);

    void GatherBatch(size_t numRows, size_t numInputs, const std::function<const Matrix<ElemType>&(size_t)>& inputs);
    void ScatterBatch(ElemType beta, size_t numRows, size_t numOutputs, const std::function<Matrix<ElemType>&(size_t)>& outputs) const;

    Matrix<ElemType> Diagonal() const;
    void AssignDiagonalValuesTo(Matrix<ElemType>& diag) const;

    void SGDUpdate(Matrix<ElemType>& gradients, ElemType learnRatePerSample);
    void MomentumSGDUpdate(Matrix<ElemType>& gradients, Matrix<ElemType>& smoothedGradients, ElemType learnRatePerSample, ElemType momentum, ElemType unitGainFactor);
    void NesterovAcceleratedMomentumSGDUpdate(Matrix<ElemType>& gradients, Matrix<ElemType>& smoothedGradients, ElemType learnRatePerSample, ElemType momentum, ElemType unitGainFactor);

    ElemType Adagrad(Matrix<ElemType>& gradients, const bool needAveMultiplier);
    void FSAdagradUpdate(Matrix<ElemType>& gradients, Matrix<ElemType>& functionValues, const double targetAdagradAvDenom_x_sqrtAdagradSqrFrames,
                         const double learnRatePerSample, const double meanMomentum, const double varMomentum, ElemType unitGainFactor);

    void AdamUpdate(Matrix<ElemType>& gradients, Matrix<ElemType>& functionValues, const double smoothedCount,
        const double learnRatePerSample, const double meanMomentum, const double varMomentum, const double epsilon, ElemType unitGainFactor, size_t refMbSize = 1, size_t actualMbSize = 1, bool adamax = false);

    ElemType RmsProp(Matrix<ElemType>& gradients, ElemType RMS_GAMMA, ElemType RMS_WGT_INC, ElemType RMS_WGT_MAX, ElemType RMS_WGT_DEC, ElemType RMS_WGT_MIN, const bool needAveMultiplier, const bool initialized);

    void AdaDeltaUpdate(Matrix<ElemType>& gradients, Matrix<ElemType>& functionvalues, ElemType learningRatePerSample, ElemType rho, ElemType epsilon);

    void Resize1(const size_t numRows, const size_t numCols, const size_t numNZElemToReserve = 10000, bool growOnly = true) final; // from MatrixBase. BUGBUG: This should just be the same Resize() method, but linking fails.
    void Resize(const size_t numRows, const size_t numCols, const size_t numNZElemToReserve = 10000, bool growOnly = true); // by default we only reallocate if need to grow
    void Resize(const Matrix<ElemType>& other) // TODO: Should this carry over numNZElemToReserve for sparse matrices?
    {
        Resize(other.GetNumRows(), other.GetNumCols());
    }
    void VerifySize(size_t rows, size_t cols)
    {
        m_baseMatrix->VerifySize(rows, cols);
    }

    // TODO: Call this ShallowClone instead?
    Matrix<ElemType> AsReference() const
    {
        return ColumnSlice(0, GetNumCols());
    }                                                                           // get a reference (e.g. this is not resizable but can be reshaped)
    Matrix<ElemType>& Reshape(const size_t numRows, const size_t numCols);      // note: reshapes in place. To get a reshaped reference, use Reshaped()
    Matrix<ElemType> Reshaped(const size_t numRows, const size_t numCols) const // get a reshaped reference
    {
        Matrix<ElemType> result = AsReference();
        result.Reshape(numRows, numCols);
        return result;
    }

    // update number of columns
    // TODO: a future version may want to enforce retaining the content, to allow dynamically growing layouts column by column (when size is not known upfront)
    void ResizeColumns(const size_t numCols)
    {
        Resize(GetNumRows(), numCols);
    }

    // similarl to the repmat operation in matlab or octave
    static Matrix<ElemType> RepMat(const Matrix<ElemType>& frmMat, const size_t rows, const size_t cols);
    size_t GetAllocatedSize() const;
    void Reset() final; // reset for sparse matrix

    const ElemType operator()(const size_t row, const size_t col) const;
    ElemType& operator()(const size_t row, const size_t col);
    ElemType GetValue(const size_t row, const size_t col) const { return operator()(row, col); } // use this for reading on non-const objects to avoid inefficiency
    ElemType Get00Element() const;

    void SetValue(const ElemType v);
    void SetValue(const DeviceBoundNumber<ElemType>& db_number);
    //void SetValue       (const Matrix<ElemType>& deepCopyFrom, const MatrixFormat format = matrixFormatSparseCSR); // BUGBUG: default for 'format' is unexpected
    // SetValue respects the source matrix's information. It moves the target's location (if necessary), and then copies the sources values.
    void SetValue      (const Matrix<ElemType>& deepCopyFrom);
    // AssignValuesOf respects the target matrix's information. It copies the values from the target into the memory of the source.
    void AssignValuesOf(const Matrix<ElemType>& deepCopyFrom);
    void SetValue(const size_t numRows, const size_t numCols, int deviceId, ElemType* pArray, const size_t matrixFlags = matrixFlagNormal, DataTransferer* transferer = nullptr);
    void SetValue(const size_t rIdx, const size_t cIdx, ElemType val); // set matrix sparsely
    void SetValue(const size_t numRows, const size_t numCols, std::initializer_list<ElemType> l) // SetValue(2,3, {1,2,3,  4,5,6});
    {
        std::vector<ElemType> vals(l);
        assert(vals.size() == numRows * numCols);
        SetValue(numRows, numCols, GetDeviceId(), vals.data(), matrixFormatRowMajor);
    }
    void AssignValues(const double* data, const size_t size);
    void CastAssignValuesOf(const MatrixBase& other) final; // allows for mixed assignment with conversion
    static ElemType MakeNan(size_t payload);
    void Invalidate()
    {
        SetValue(MakeNan(__LINE__));
    }
    void SetMatrixFromCSCFormat(const CPUSPARSE_INDEX_TYPE* h_CSCCol, const CPUSPARSE_INDEX_TYPE* h_Row, const ElemType* h_Val,
        const size_t nz, const size_t numRows, const size_t numCols, DataTransferer* transferer = nullptr);

    void MaskColumnsValue(const Matrix<char>& columnsMask, ElemType val, size_t numColsPerMaskEntry);

    void SetColumn(const ElemType* colPointer, size_t colInd);
    void SetColumn(const ElemType val, size_t colInd);
    void SetColumn(const Matrix<ElemType>& valMat, size_t colInd);

    void AdjustSparseBlockColumn(const GPUSPARSE_INDEX_TYPE* cpuCol2BlockId, size_t numBlocks, bool useBlockId2Col);

    void SetDiagonalValue(const ElemType v);
    void SetDiagonalValue(const Matrix<ElemType>& vector);
    void SetUniformRandomValue(const ElemType low, const ElemType high, unsigned long seed = USE_TIME_BASED_SEED);
    void SetUniformRandomValue(RNGHandle& rngHandle, const ElemType low, const ElemType high);
    void SetGaussianRandomValue(RNGHandle& rngHandle, const ElemType mean, const ElemType stdev);
    void SetGumbelRandomValue(RNGHandle& rngHandle, const ElemType loc, const ElemType scale);
    void SetGaussianRandomValue(const ElemType mean, const ElemType sigma, unsigned long seed = USE_TIME_BASED_SEED);
    void SetTruncatedNormalRandomValue(const ElemType mean, const ElemType sigma, unsigned long seed = USE_TIME_BASED_SEED);
    void SetUniformRandomMask(const ElemType maskRate, const ElemType scaleValue, RNGHandle& rngHandle);
    void AddGaussianRandomValue(const ElemType mean, const ElemType sigma, unsigned long seed = USE_TIME_BASED_SEED);
    Matrix<ElemType>& AssignNoiseContrastiveEstimation(const Matrix<ElemType>& a, const Matrix<ElemType>& b, const Matrix<ElemType>& c, const Matrix<ElemType>& bias, Matrix<ElemType>& tmp);

    Matrix<ElemType>& AssignNCEDerivative(const Matrix<ElemType>& tmp, const Matrix<ElemType>& a, const Matrix<ElemType>& b, const Matrix<ElemType>& c, size_t inputIndex);
    Matrix<ElemType>& AssignSoftmaxSum(const Matrix<ElemType>& a, const Matrix<ElemType>& softmax);
    Matrix<ElemType>& AssignNceUnnormalizedEval(const Matrix<ElemType>& a, const Matrix<ElemType>& b, const Matrix<ElemType>& c, const Matrix<ElemType>& bias);

    Matrix<ElemType>& AssignOneHot(const Matrix<ElemType>& a, vector<size_t>& shape, size_t axis, bool is_sparse);
    Matrix<ElemType>& GatherFromTarget(const Matrix<ElemType>& indices, const Matrix<ElemType>& target, size_t row_elements);
    Matrix<ElemType>& ScatterToIndices(const Matrix<ElemType>& values, const Matrix<ElemType>& indices, size_t row_elements);

    Matrix<ElemType> Transpose(); // This method doesn't change state of Matrix. It should be a const function
    Matrix<ElemType>& AssignTransposeOf(const Matrix<ElemType>& a);

    Matrix<ElemType>& DoGatherColumnsOf (ElemType beta, const Matrix<ElemType>& idx, const Matrix<ElemType>& a, ElemType alpha);
    Matrix<ElemType>& DoScatterColumnsOf(ElemType beta, const Matrix<ElemType>& idx, const Matrix<ElemType>& a, ElemType alpha);

    Matrix<ElemType>& operator+=(const ElemType alpha);
    Matrix<ElemType>  operator+(const ElemType alpha) const;
    Matrix<ElemType>& AssignSumOf(const ElemType alpha, const Matrix<ElemType>& a);

    Matrix<ElemType>& operator+=(const Matrix<ElemType>& a);
    Matrix<ElemType>  operator+(const Matrix<ElemType>& a) const;
    Matrix<ElemType>& AssignSumOf(const Matrix<ElemType>& a, const Matrix<ElemType>& b);

    Matrix<ElemType>& operator-=(const ElemType alpha);
    Matrix<ElemType>  operator-(const ElemType alpha) const;
    Matrix<ElemType>& AssignDifferenceOf(const ElemType alpha, const Matrix<ElemType>& a);
    Matrix<ElemType>& AssignDifferenceOf(const Matrix<ElemType>& a, const ElemType alpha);

    Matrix<ElemType>& operator-=(const Matrix<ElemType>& a);
    Matrix<ElemType>  operator-(const Matrix<ElemType>& a) const;
    Matrix<ElemType>& AssignDifferenceOf(const Matrix<ElemType>& a, const Matrix<ElemType>& b);

    Matrix<ElemType>& operator*=(const ElemType alpha);
    Matrix<ElemType>  operator*(const ElemType alpha) const;
    Matrix<ElemType>& AssignProductOf(const ElemType alpha, const Matrix<ElemType>& a);

    Matrix<ElemType>  operator*(const Matrix<ElemType>& a) const;
    Matrix<ElemType>& AssignProductOf(const Matrix<ElemType>& a, const bool transposeA, const Matrix<ElemType>& b, const bool transposeB); // this = a * b
    Matrix<ElemType>& Assign1x1ProductOf(const Matrix<ElemType>& a1x1, const Matrix<ElemType>& b);                                         // this = a * b, where a is 1x1

    Matrix<ElemType>& operator/=(ElemType alpha);
    Matrix<ElemType>  operator/(ElemType alpha) const;

    Matrix<ElemType>& operator^=(ElemType alpha);     // element-wise power
    Matrix<ElemType>  operator^(ElemType alpha) const; // element-wise power
    Matrix<ElemType>& AssignElementPowerOf(const Matrix<ElemType>& a, const ElemType power);

    // TODO: There are several functions below that perform an in-place operation
    // We should prepend the names of these functions with InPlace for clearly indicating
    // the semantics for callers.
    Matrix<ElemType>& ElementMultiplyWith(const Matrix<ElemType>& a);
    Matrix<ElemType>& AssignElementProductOf(const Matrix<ElemType>& a, const Matrix<ElemType>& b);
    Matrix<ElemType>& AddElementProductOf(const Matrix<ElemType>& a, const Matrix<ElemType>& b);

    Matrix<ElemType>& AssignElementDivisionOf(const Matrix<ElemType>& a, const Matrix<ElemType>& b);
    Matrix<ElemType>& ElementDivideBy(const Matrix<ElemType>& a);

    Matrix<ElemType>& ColumnElementMultiplyWith(const Matrix<ElemType>& a);
    Matrix<ElemType>& RowElementMultiplyWith(const Matrix<ElemType>& a);

    Matrix<ElemType>& ColumnElementDivideBy(const Matrix<ElemType>& a);
    Matrix<ElemType>& RowElementDivideBy(const Matrix<ElemType>& a);

    Matrix<ElemType>& ElementInverse();
    Matrix<ElemType>& AssignElementInverseOf(const Matrix<ElemType>& a);

    Matrix<ElemType>& InplaceLinearRectifierDerivative();
    Matrix<ElemType>& AssignLinearRectifierDerivativeOf(const Matrix<ElemType>& a);

    Matrix<ElemType>& InplaceSigmoidDerivative();
    Matrix<ElemType>& AssignSigmoidDerivativeOf(const Matrix<ElemType>& a);

    Matrix<ElemType>& InplaceSigmoid();
    Matrix<ElemType>& AssignSigmoidOf(const Matrix<ElemType>& a);

    Matrix<ElemType>& InplaceTanh();
    Matrix<ElemType>& AssignTanhOf(const Matrix<ElemType>& a);

    Matrix<ElemType>& InplaceLogSoftmax(const bool isColWise);
    Matrix<ElemType>& AssignLogSoftmaxOf(const Matrix<ElemType>& a, const bool isColWise);

    Matrix<ElemType>& InplaceHardmax(const bool isColWise);
    Matrix<ElemType>& AssignHardmaxOf(const Matrix<ElemType>& a, const bool isColWise);

    Matrix<ElemType>& AssignColumnwiseArgmaxOf(const Matrix<ElemType>& a);

    // sequence training
    Matrix<ElemType>& DropFrame(const Matrix<ElemType>& label, const Matrix<ElemType>& gamma, const ElemType& threshhold);
    Matrix<ElemType>& AssignSequenceError(const ElemType hsmoothingWeight, const Matrix<ElemType>& label, const Matrix<ElemType>& dnnoutput, const Matrix<ElemType>& gamma, ElemType alpha);

    Matrix<ElemType>& AssignCTCScore(const Matrix<ElemType>& prob, Matrix<ElemType>& alpha, Matrix<ElemType>& beta, const Matrix<ElemType>& phoneSeq, const Matrix<ElemType>& phoneBound, Matrix<ElemType>& totalScore,
        const vector<size_t> & extraUttMap, const vector<size_t> & uttBeginFrame, const vector<size_t> & uttFrameNum, const vector<size_t> & uttPhoneNum, const size_t samplesInRecurrentStep,
        const size_t mbSize, const size_t blankTokenId, const int delayConstraint, const bool isColWise);

    Matrix<ElemType>& InplaceSqrt();
    Matrix<ElemType>& AssignSqrtOf(const Matrix<ElemType>& a);

    Matrix<ElemType>& InplaceExp();
    Matrix<ElemType>& AssignExpOf(const Matrix<ElemType>& a);

    Matrix<ElemType>& InplaceLog();
    Matrix<ElemType>& AssignLogOf(const Matrix<ElemType>& a);

    Matrix<ElemType>& InplaceCosine();
    Matrix<ElemType>& AssignCosineOf(const Matrix<ElemType>& a);

    Matrix<ElemType>& InplaceNegativeSine();
    Matrix<ElemType>& AssignNegativeSineOf(const Matrix<ElemType>& a);

    Matrix<ElemType>& InplaceAcos();
    Matrix<ElemType>& AssignAcosOf(const Matrix<ElemType>& a);

    Matrix<ElemType>& InplaceAsin();
    Matrix<ElemType>& AssignAsinOf(const Matrix<ElemType>& a);

    Matrix<ElemType>& InplaceCosh();
    Matrix<ElemType>& AssignCoshOf(const Matrix<ElemType>& a);

    Matrix<ElemType>& InplaceSinh();
    Matrix<ElemType>& AssignSinhOf(const Matrix<ElemType>& a);

    Matrix<ElemType>& InplaceLog10();
    Matrix<ElemType>& AssignLog10Of(const Matrix<ElemType>& a);

    Matrix<ElemType>& InplaceAbs();
    Matrix<ElemType>& AssignAbsOf(const Matrix<ElemType>& a);

    // TODO: rename these to InPlaceFloor() and -Ceil() (I never know what it means to truncate a bottom)
    //       And also document and implement that sparse matrices can only truncate towards 0.
    Matrix<ElemType>& InplaceTruncateBottom(const ElemType threshold);
    Matrix<ElemType>& AssignTruncateBottomOf(const Matrix<ElemType>& a, const ElemType threshold);
    Matrix<ElemType>& InplaceTruncateTop(const ElemType threshold);
    Matrix<ElemType>& AssignTruncateTopOf(const Matrix<ElemType>& a, const ElemType threshold);
    Matrix<ElemType>& InplaceTruncate(const ElemType threshold);
    Matrix<ElemType>& InplaceSoftThreshold(const ElemType threshold);
    void InplaceTranspose();

    Matrix<ElemType>& SetToZeroIfAbsLessThan(const ElemType threshold);

    DeviceBoundNumber<ElemType> Sum_AsDeviceBoundNum() const;
    ElemType SumOfAbsElements() const; // sum of all abs(elements)
    ElemType SumOfElements() const;    // sum of all elements
    Matrix<ElemType>& AssignSumOfElements(const Matrix<ElemType>& a);

    ElemType LogSumOfElements() const;

    Matrix<ElemType>& AssignToRowSliceValuesOf(const Matrix<ElemType>& a, const size_t startIndex, const size_t numRows);
    Matrix<ElemType>& AssignRowSliceValuesOf(const Matrix<ElemType>& a, const size_t startIndex, const size_t numRows);
    Matrix<ElemType>& AddToRowSliceValuesOf(const Matrix<ElemType>& a, const size_t startIndex, const size_t numRows);
    Matrix<ElemType>& AddWithRowSliceValuesOf(const Matrix<ElemType>& a, const size_t startIndex, const size_t numRows);
    // Matrix<ElemType>&  AssignRowStackValuesOf(const std::vector<const Matrix<ElemType>*>& inputMatrices, const size_t sliceStartCol, const size_t sliceNumCols);

    Matrix<ElemType>& AssignRepeatOf(const Matrix<ElemType>& a, const size_t numRowRepeats, const size_t numColRepeats);
    Matrix<ElemType>& AddToRowRepeatValuesOf(const Matrix<ElemType>& a, const size_t numRepeats);

    Matrix<ElemType>& AssignPositiveAndShiftedNegSample(const Matrix<ElemType>& a, const size_t posNumber, const size_t negNumber, const size_t shiftNumber);
    Matrix<ElemType>& AddFoldedPositiveAndShiftedNegSample(const Matrix<ElemType>& a, const size_t posNumber, const size_t negNumber, const size_t shiftNumber);

    bool IsValid() const;
    bool IsEqualTo(const Matrix<ElemType>& a, const ElemType threshold = 1e-8) const;

    static void VectorSum(const Matrix<ElemType>& a, Matrix<ElemType>& c, const bool isColWise);

    void VectorNorm1(Matrix<ElemType>& c, const bool isColWise) const;
    Matrix<ElemType>& AssignVectorNorm1Of(Matrix<ElemType>& a, const bool isColWise); // TODO: arg should be const

    void VectorNorm2(Matrix<ElemType>& c, const bool isColWise) const;
    Matrix<ElemType>& AssignVectorNorm2Of(Matrix<ElemType>& a, const bool isColWise); // TODO: arg should be const

    void VectorNormInf(Matrix<ElemType>& c, const bool isColWise) const;
    Matrix<ElemType>& AssignVectorNormInfOf(Matrix<ElemType>& a, const bool isColWise);

    Matrix<ElemType>& AssignInnerProductOf(const Matrix<ElemType>& a, const Matrix<ElemType>& b, const bool isColWise);
    Matrix<ElemType>& AssignKhatriRaoProductOf(const Matrix<ElemType>& a, const Matrix<ElemType>& b);
    Matrix<ElemType>& AddColumnReshapeProductOf(const Matrix<ElemType>& a, const Matrix<ElemType>& b, const bool transposeAColumn);

    Matrix<ElemType>& AddWithScaleOf(ElemType alpha, const Matrix<ElemType>& a); // this += alpha * a

    ElemType FrobeniusNorm() const;
    Matrix<ElemType>& AssignFrobeniusNormOf(const Matrix<ElemType>& a);

    ElemType MatrixNormInf() const;
    ElemType MatrixNorm1() const;
    ElemType MatrixNorm0() const; // number of non-zero elemets
    Matrix<ElemType>& AssignSignOf(const Matrix<ElemType>& a);
    Matrix<ElemType>& AddSignOf(const Matrix<ElemType>& a);
    void VectorMax(Matrix<ElemType>& maxIndexes, Matrix<ElemType>& maxValues, const bool isColWise) const;
    void VectorMax(Matrix<ElemType>& maxIndexes, Matrix<ElemType>& maxValues, const bool isColWise, int topK) const;
    void VectorMin(Matrix<ElemType>& minIndexes, Matrix<ElemType>& minValues, const bool isColWise) const;

    Matrix<ElemType>& AssignNumOfDiff(const Matrix<ElemType>& a, const Matrix<ElemType>& b, bool searchInCol = false);

    Matrix<ElemType>& AssignInnerProductOfMatrices(const Matrix<ElemType>& a, const Matrix<ElemType>& b); // this method will resize(1,1) first

    bool HasNan(const char* name) const;
    size_t CountNanInf() const;

    void Print(const char* matrixName, ptrdiff_t rowFirst, ptrdiff_t rowLast, ptrdiff_t colFirst, ptrdiff_t colLast) const;
    void Print(const char* matrixName = nullptr) const; // print whole matrix. can be expensive

    Matrix<ElemType>& AssignPackedConvolutionInput(const Matrix<ElemType>& inputSubBatch,
                                                   const size_t inputWidth, const size_t inputHeight, const size_t inputChannels,
                                                   const size_t outputWidth, const size_t outputHeight, const size_t outputChannels,
                                                   const size_t kernelWidth, const size_t kernelHeight, const size_t horizontalSubsample, const size_t verticalSubsample,
                                                   const bool zeroPadding = false);
    Matrix<ElemType>& UnpackConvolutionInput(Matrix<ElemType>& inputSubBatch,
                                             const size_t inputWidth, const size_t inputHeight, const size_t inputChannels,
                                             const size_t outputWidth, const size_t outputHeight, const size_t outputChannels,
                                             const size_t kernelWidth, const size_t kernelHeight, const size_t horizontalSubsample, const size_t verticalSubsample,
                                             const bool zeroPadding = false) const;
    Matrix<ElemType>& AssignMaxPoolingResult(const Matrix<ElemType>& inputBatch, const size_t channels,
                                             const size_t inputWidth, const size_t inputHeight, const size_t inputSizePerSample,
                                             const size_t outputWidth, const size_t outputHeight, const size_t outputSizePerSample,
                                             const size_t windowWidth, const size_t windowHeight, const size_t horizontalSubsample, const size_t verticalSubsample);
    Matrix<ElemType>& AddMaxPoolingGradient(const Matrix<ElemType>& outputGradientBatch, const Matrix<ElemType>& inputBatch, const Matrix<ElemType>& outputBatch,
                                            const size_t channels,
                                            const size_t inputWidth, const size_t inputHeight, const size_t inputSizePerSample,
                                            const size_t outputWidth, const size_t outputHeight, const size_t outputSizePerSample,
                                            const size_t windowWidth, const size_t windowHeight, const size_t horizontalSubsample, const size_t verticalSubsample);
    Matrix<ElemType>& AssignAveragePoolingResult(const Matrix<ElemType>& inputBatch, const size_t channels,
                                                 const size_t inputWidth, const size_t inputHeight, const size_t inputSizePerSample,
                                                 const size_t outputWidth, const size_t outputHeight, const size_t outputSizePerSample,
                                                 const size_t windowWidth, const size_t windowHeight, const size_t horizontalSubsample, const size_t verticalSubsample);
    Matrix<ElemType>& AddAveragePoolingGradient(const Matrix<ElemType>& outputGradientBatch,
                                                const size_t channels,
                                                const size_t inputWidth, const size_t inputHeight, const size_t inputSizePerSample,
                                                const size_t outputWidth, const size_t outputHeight, const size_t outputSizePerSample,
                                                const size_t windowWidth, const size_t windowHeight, const size_t horizontalSubsample, const size_t verticalSubsample);

    void ConvolutionForward(const Matrix<ElemType>& kernel, const Matrix<int>& mpRowCol, const Matrix<int>& mpRowIwht,
                            const Matrix<int>& mpRowRun, const Matrix<int>& runs, Matrix<ElemType>& output) const;
    void ConvolutionBackwardData(const Matrix<ElemType>& kernel, const Matrix<int>& mpRowCol, const Matrix<int>& mpRowIwht,
                                 const Matrix<int>& mpRowRun, const Matrix<int>& runs, Matrix<ElemType>& grad) const;
    void ConvolutionBackwardKernel(const Matrix<ElemType>& in, const Matrix<int>& mpRowCol, const Matrix<int>& mpRowIwht,
                                   const Matrix<int>& mpRowRun, const Matrix<int>& runs, Matrix<ElemType>& kernelGrad) const;

    void UnrollConvolutionInput(size_t unrollCols, size_t mapOutSize, const Matrix<int>& mpRowCol,
                                const Matrix<int>& mpRowRun, const Matrix<int>& runs, Matrix<ElemType>& output) const;
    void UnrollConvolutionOutput(size_t unrollCols, size_t mapInCount, size_t mapOutCount, const Matrix<int>& mpRowCol,
                                 const Matrix<int>& mpRowRun, const Matrix<int>& runs, Matrix<ElemType>& output) const;
    void UnrollConvolutionInputForKernelBackprop(size_t mapOutSize, const Matrix<int>& mpRowCol,
                                                 const Matrix<int>& mpRowRun, const Matrix<int>& runs, Matrix<ElemType>& output) const;

    void MaxPoolingForward(const Matrix<int>& mpRowCol, const Matrix<int>& mpRowIndices, const Matrix<int>& indices, Matrix<ElemType>& output) const;
    void MaxPoolingBackward(const Matrix<ElemType>& out, const Matrix<ElemType>& in,
                            const Matrix<int>& mpRowCol, const Matrix<int>& mpRowIndices, const Matrix<int>& indices,
                            Matrix<ElemType>& grad, bool accumulateGradient) const;

    void MaxROIPoolingForward(const size_t numRois, const size_t numImg, const size_t channels, const size_t width, const size_t height,
                              const size_t pooledWidth, const size_t pooledHeight, const Matrix<ElemType>& roiData, Matrix<ElemType>& output, Matrix<ElemType>& argmax, double spatialScale) const;

    void MaxROIPoolingBackward(const size_t numRois, const size_t numImg, const size_t channels, const size_t width, const size_t height,
                               const size_t pooledWidth, const size_t pooledHeight, const Matrix<ElemType>& roiData, Matrix<ElemType>& grad, Matrix<ElemType>& argmax, double spatialScale) const;

    void MaxUnpooling(const Matrix<int>& mpRowCol, const Matrix<int>& mpRowIndices, const Matrix<int>& indices, const Matrix<ElemType>& poolInput, Matrix<ElemType>& input) const;

    void AveragePoolingForward(const Matrix<int>& mpRowCol, const Matrix<int>& mpRowIndices, const Matrix<int>& indices, Matrix<ElemType>& output, const bool poolIncludePad) const;
    void AveragePoolingBackward(const Matrix<int>& mpRowCol, const Matrix<int>& mpRowIndices, const Matrix<int>& indices, Matrix<ElemType>& grad, const bool poolIncludePad, bool accumulateGradient) const;

    void BatchNormalizationForward(const Matrix<ElemType>& scale, const Matrix<ElemType>& bias, bool inferenceOnly, double expAvgFactor, double blendFactor,
                                   Matrix<ElemType>& runMean, Matrix<ElemType>& runVariance, Matrix<ElemType>& out, double epsilon,
                                   Matrix<ElemType>& saveMean, Matrix<ElemType>& saveInvStdDev) const;
    void BatchNormalizationBackward(const Matrix<ElemType>& in, Matrix<ElemType>& grad, const Matrix<ElemType>& scale, double blendFactor, const Matrix<ElemType>& saveMean, const Matrix<ElemType>& saveInvStdDev,
                                    Matrix<ElemType>& scaleGrad, Matrix<ElemType>& biasGrad) const;

    void RNNForward(const Matrix<ElemType>& inputX, const Matrix<ElemType>& paramW, size_t xDim, size_t yDim, const vector<size_t>& numSequencesForFrame, const struct RnnAttributes& rnnAttributes, Matrix<ElemType>& reserve, Matrix<ElemType>& workspace);
    void RNNBackwardData(const Matrix<ElemType>& outputDY, const Matrix<ElemType>& paramW, Matrix<ElemType>& outputDX, const struct RnnAttributes& rnnAttributes, Matrix<ElemType>& reserve, Matrix<ElemType>& workspace);
    void RNNBackwardWeights(const Matrix<ElemType>& inputX, const Matrix<ElemType>& outputY, Matrix<ElemType>& dw, const struct RnnAttributes& rnnAttributes, Matrix<ElemType>& reserve, Matrix<ElemType>& workspace);

public:
    // TODO: why are these not static? And why are they here?
    ElemType Exp10(ElemType num);
    ElemType Mod(ElemType x, ElemType y);
    ElemType LogAdd(ElemType x, ElemType y);

public:
    // static BLAS functions

    // singular value decomposition of A as A = U*SIGMA*VT
    static void SVD(const Matrix<ElemType>& A, Matrix<ElemType>& SIGMA, Matrix<ElemType>& U, Matrix<ElemType>& VT, Matrix<ElemType>& W);

    static void MultiplyAndWeightedAdd(ElemType alpha, const Matrix<ElemType>& a, const bool transposeA, const Matrix<ElemType>& b, const bool transposeB, ElemType beta, Matrix<ElemType>& c, shared_ptr<QuantizedMultiplier<ElemType>> pQuantizedMultiplier=nullptr); // SGEMM
    static void MultiplyAndAdd(const Matrix<ElemType>& a, const bool transposeA, const Matrix<ElemType>& b, const bool transposeB, Matrix<ElemType>& c);
    static void Multiply(const Matrix<ElemType>& a, const bool transposeA, const Matrix<ElemType>& b, const bool transposeB, Matrix<ElemType>& c);
    static void Multiply(const Matrix<ElemType>& a, const Matrix<ElemType>& b, Matrix<ElemType>& c);
    static void Multiply1x1AndWeightedAdd(ElemType alpha, const Matrix<ElemType>& a, const Matrix<ElemType>& b, ElemType beta, Matrix<ElemType>& c);
    static void ConvolveAndWeightedAdd(ElemType alpha, const Matrix<ElemType>& a, const bool transposeA, const Matrix<ElemType>& b, const bool transposeB, ElemType beta, Matrix<ElemType>& c, size_t numChannels, size_t horizontalSubsample, bool padding, bool channelwise);

    static void ColumnwiseScaleAndWeightedAdd(ElemType alpha, const Matrix<ElemType>& a, const Matrix<ElemType>& v, ElemType beta, Matrix<ElemType>& c);

    static void ScaleAndAdd(ElemType alpha, const Matrix<ElemType>& a, Matrix<ElemType>& c);
    static void ScaleAndAdd(ElemType alpha, const Matrix<ElemType>& a, ElemType beta, Matrix<ElemType>& c);
    static void AddScaledDifference(const ElemType alpha, const Matrix<ElemType>& a, const Matrix<ElemType>& b, Matrix<ElemType>& c);
    static void AssignScaledDifference(const ElemType alpha, const Matrix<ElemType>& a, const Matrix<ElemType>& b, Matrix<ElemType>& c);
    static void AddScaledDifference(const Matrix<ElemType>& alpha, const Matrix<ElemType>& a, const Matrix<ElemType>& b, Matrix<ElemType>& c); // c += alpha * (a - b)
    static void AssignScaledDifference(const Matrix<ElemType>& alpha, const Matrix<ElemType>& a, const Matrix<ElemType>& b, Matrix<ElemType>& c);

    static void AddElementToElement(const Matrix<ElemType>& a, const size_t ai, const size_t aj, Matrix<ElemType>& c, const size_t ci, const size_t cj);
    // static void AddLogElementToElement(const Matrix<ElemType>& a, const size_t ai, const size_t aj, Matrix<ElemType>& c, const size_t ci, const size_t cj);
    static void AssignElementToElement(const Matrix<ElemType>& a, const size_t ai, const size_t aj, Matrix<ElemType>& c, const size_t ci, const size_t cj);
    static void MinusOneAt(Matrix<ElemType>& c, const size_t position);

    static void Scale(ElemType alpha, Matrix<ElemType>& a);
    static void Scale(const Matrix<ElemType>& alpha, Matrix<ElemType>& a); // In this case Matrix alpha must be 1x1
    static void Scale(ElemType alpha, const Matrix<ElemType>& a, Matrix<ElemType>& c);
    static void InnerProduct(const Matrix<ElemType>& a, const Matrix<ElemType>& b, Matrix<ElemType>& c, const bool isColWise);
    static ElemType InnerProductOfMatrices(const Matrix<ElemType>& a, const Matrix<ElemType>& b);
    static void ElementWisePower(ElemType alpha, const Matrix<ElemType>& a, Matrix<ElemType>& c);

    static bool AreEqual(const Matrix<ElemType>& a, const Matrix<ElemType>& b, const ElemType threshold = 1e-8);
    static bool HasElement(const Matrix<ElemType>& a, const ElemType value = 0.0);

    static void TensorShuffleScaleAndAdd(ElemType keepWeight, const Matrix<ElemType>& a, size_t D, size_t S, size_t M, size_t K, size_t T, ElemType scaleFactor, const Matrix<ElemType>& b, Matrix<ElemType>& c);

    template<size_t N>
    static void TensorOp(size_t arity, const std::array<std::reference_wrapper<Matrix>, N>& args, ElementWiseOperator op, ElementWiseOperator reductionOp, ElemType alpha, ElemType beta,
                         const std::array<size_t, N>& offsets,
                         const SmallVector<size_t>& regularOpDims,  const std::array<SmallVector<ptrdiff_t>, N>& regularStrides,
                         const SmallVector<size_t>& reducingOpDims, const std::array<SmallVector<ptrdiff_t>, N>& reducingStrides);

    // TODO: absorb in TensorOp above
    void TensorArgOp(const Matrix<ElemType>& a, ElementWiseOperator reductionOp,
                     const std::array<size_t, 2>& offsets,
                     const SmallVector<size_t>& regularOpDims, const std::array<SmallVector<ptrdiff_t>, 2>& regularStrides,
                     const SmallVector<size_t>& reducingOpDims, const std::array<SmallVector<ptrdiff_t>, 2>& reducingStrides);

public:
    void Read(File& stream);
    void Write(File& stream) const;

    Matrix<ElemType>& Shift(const Matrix<ElemType>& a, int shift);

    Matrix<ElemType>& AssignElementProductOfWithShiftNeg(const Matrix<ElemType>& a, const Matrix<ElemType>& b, size_t shift, size_t negnumber);
    Matrix<ElemType>& AssignInnerProductOfWithShiftNeg(const Matrix<ElemType>& a, const Matrix<ElemType>& b, const bool isColWise, size_t shift, size_t negnumber);
    static void InnerProductWithShiftNeg(const Matrix<ElemType>& a, const Matrix<ElemType>& b, Matrix<ElemType>& c, const bool isColWise, size_t shift, size_t negnumber);
    Matrix<ElemType>& GetARowByIndex(const Matrix<ElemType>& a, size_t index);
    static void ConductRowElementMultiplyWithShift(const Matrix<ElemType>& a, const Matrix<ElemType>& b, Matrix<ElemType>& c, size_t shift, bool bFirstmatrixfixed);
    Matrix<ElemType>& AssignElementProductOfWithShift(const Matrix<ElemType>& a, const Matrix<ElemType>& b, size_t shift);

public:
    static void RCRFBackwardCompute(const Matrix<ElemType>& alpha, Matrix<ElemType>& beta,
                                    Matrix<ElemType>& functionValues, const Matrix<ElemType>& lbls,
                                    const Matrix<ElemType>& pos_scores, const Matrix<ElemType>& pair_scores, const int shift);

    static void RCRFTransGrdCompute(const Matrix<ElemType>& lbls,
                                    const Matrix<ElemType>& alpha,
                                    const Matrix<ElemType>& beta,
                                    const Matrix<ElemType>& pair_scores,
                                    Matrix<ElemType>& grd,
                                    const int startLbl, // the time 0 start symbol in the output layer
                                    const int shift);

    template <typename T>
    friend class MatrixQuantizer;

    template <typename T>
    friend class QuantizedMatrix;

    template <typename T>
    friend class Matrix;
};

// overload I/O operators
template <class ElemType>
File& operator>>(File& stream, Matrix<ElemType>& M)
{
    M.Read(stream);
    return stream;
}
template <class ElemType>
File& operator<<(File& stream, const Matrix<ElemType>& M)
{
    M.Write(stream);
    return stream;
}

typedef Matrix<float> SingleMatrix;
typedef Matrix<double> DoubleMatrix;

}}}
